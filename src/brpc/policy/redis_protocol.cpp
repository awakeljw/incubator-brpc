// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Authors: Ge,Jun (gejun@baidu.com)
//          Jiashun Zhu(zhujiashun2010@gmail.com)

#include <google/protobuf/descriptor.h>         // MethodDescriptor
#include <google/protobuf/message.h>            // Message
#include <gflags/gflags.h>
#include "butil/logging.h"                       // LOG()
#include "butil/time.h"
#include "butil/iobuf.h"                         // butil::IOBuf
#include "brpc/controller.h"               // Controller
#include "brpc/details/controller_private_accessor.h"
#include "brpc/socket.h"                   // Socket
#include "brpc/server.h"                   // Server
#include "brpc/details/server_private_accessor.h"
#include "brpc/span.h"
#include "brpc/redis.h"
#include "brpc/redis_command.h"
#include "brpc/policy/redis_protocol.h"
#include "bthread/execution_queue.h"

namespace brpc {

DECLARE_bool(enable_rpcz);
DECLARE_bool(usercode_in_pthread);

namespace policy {

DEFINE_bool(redis_verbose, false,
            "[DEBUG] Print EVERY redis request/response");
DEFINE_int32(redis_batch_flush_data_size, 4096, "If the total data size of buffered "
        "responses is beyond this value, then data is forced to write to socket"
        "to avoid latency of the front responses being too big");

struct InputResponse : public InputMessageBase {
    bthread_id_t id_wait;
    RedisResponse response;

    // @InputMessageBase
    void DestroyImpl() {
        delete this;
    }
};

// This class is as parsing_context in socket.
class RedisConnContext : public Destroyable  {
public:
    RedisConnContext()
        : redis_service(NULL)
        , handler_continue(NULL) {}
    ~RedisConnContext();
    // @Destroyable
    void Destroy() override;

    int Init();

    SocketId socket_id;
    RedisService* redis_service;
    // If user starts a transaction, handler_continue indicates the
    // first handler pointer that triggers the transaction.
    RedisCommandHandler* handler_continue;
    // The redis command are parsed and pushed into this queue
    bthread::ExecutionQueueId<std::string*> queue;

    RedisCommandParser parser;
};

int ConsumeTask(RedisConnContext* ctx, std::string* command, butil::IOBuf* sendbuf) {
    butil::Arena arena;
    RedisReply output(&arena);
    if (ctx->handler_continue) {
        RedisCommandHandler::Result result =
            ctx->handler_continue->Run(command->c_str(), &output);
        if (result == RedisCommandHandler::OK) {
            ctx->handler_continue = NULL;
        }
    } else {
        std::string comm;
        comm.reserve(8);
        for (int i = 0; i < (int)command->size() && (*command)[i] != ' '; ++i) {
            comm.push_back(std::tolower((*command)[i]));
        }
        RedisCommandHandler* ch = ctx->redis_service->FindCommandHandler(comm);
        if (!ch) {
            char buf[64];
            snprintf(buf, sizeof(buf), "ERR unknown command `%s`", comm.c_str());
            output.SetError(buf);
        } else {
            RedisCommandHandler::Result result = ch->Run(command->c_str(), &output);
            if (result == RedisCommandHandler::CONTINUE) {
                ctx->handler_continue = ch;
            }
        }
    }
    output.SerializeToIOBuf(sendbuf);
    return 0;
}

int Consume(void* ctx, bthread::TaskIterator<std::string*>& iter) {
    RedisConnContext* qctx = static_cast<RedisConnContext*>(ctx);
    if (iter.is_queue_stopped()) {
        delete qctx;
        return 0;
    }
    SocketUniquePtr s;
    bool has_err = false;
    if (Socket::Address(qctx->socket_id, &s) != 0) {
        LOG(WARNING) << "Fail to address redis socket";
        has_err = true;
    }
    Socket::WriteOptions wopt;
    wopt.ignore_eovercrowded = true;
    butil::IOBuf sendbuf;
    for (; iter; ++iter) {
        std::unique_ptr<std::string> guard(*iter);
        if (has_err) {
            continue;
        }
        ConsumeTask(qctx, *iter, &sendbuf);
        // If there are too many tasks to execute, latency of the front
        // responses will be increased by waiting the following tasks to
        // be completed. To prevent this, if the current buf size is greater
        // than FLAGS_redis_batch_flush_max_size, we just write the current
        // buf first.
        if ((int)sendbuf.size() >= FLAGS_redis_batch_flush_data_size) {
            LOG_IF(WARNING, s->Write(&sendbuf, &wopt) != 0)
                << "Fail to send redis reply";
        }
    }
    if (!has_err && !sendbuf.empty()) {
        LOG_IF(WARNING, s->Write(&sendbuf, &wopt) != 0)
            << "Fail to send redis reply";
    }
    return 0;
}

// ========== impl of RedisConnContext ==========

RedisConnContext::~RedisConnContext() { }

void RedisConnContext::Destroy() {
    bthread::execution_queue_stop(queue);
}

int RedisConnContext::Init() {
    bthread::ExecutionQueueOptions q_opt;
    q_opt.bthread_attr =
        FLAGS_usercode_in_pthread ? BTHREAD_ATTR_PTHREAD : BTHREAD_ATTR_NORMAL;
    if (bthread::execution_queue_start(&queue, &q_opt, Consume, this) != 0) {
        LOG(ERROR) << "Fail to start execution queue";
        return -1;
    }
    return 0;
}

// ========== impl of RedisConnContext ==========

ParseResult ParseRedisMessage(butil::IOBuf* source, Socket* socket,
                              bool read_eof, const void* arg) {
    if (read_eof || source->empty()) {
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    const Server* server = static_cast<const Server*>(arg);
    if (server) {
        RedisService* rs = server->options().redis_service;
        if (!rs) {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
        RedisConnContext* ctx = static_cast<RedisConnContext*>(socket->parsing_context());
        if (ctx == NULL) {
            ctx = new RedisConnContext;
            ctx->socket_id = socket->id();
            ctx->redis_service = rs;
            if (ctx->Init() != 0) {
                delete ctx;
                LOG(ERROR) << "Fail to init redis RedisConnContext";
                return MakeParseError(PARSE_ERROR_NO_RESOURCE);
            }
            socket->reset_parsing_context(ctx);
        }
        ParseError err = ctx->parser.ParseCommand(*source);
        if (err != PARSE_OK) {
            return MakeParseError(err);
        }
        std::unique_ptr<std::string> command(new std::string);
        command->swap(ctx->parser.Command());
        if (bthread::execution_queue_execute(ctx->queue, command.get()) != 0) {
            LOG(ERROR) << "Fail to push execution queue";
            return MakeParseError(PARSE_ERROR_NO_RESOURCE);
        }
        command.release();
        return MakeMessage(NULL);
    } else {
        // NOTE(gejun): PopPipelinedInfo() is actually more contended than what
        // I thought before. The Socket._pipeline_q is a SPSC queue pushed before
        // sending and popped when response comes back, being protected by a
        // mutex. Previously the mutex is shared with Socket._id_wait_list. When
        // 200 bthreads access one redis-server, ~1.5s in total is spent on
        // contention in 10-second duration. If the mutex is separated, the time
        // drops to ~0.25s. I further replaced PeekPipelinedInfo() with
        // GivebackPipelinedInfo() to lock only once(when receiving response)
        // in most cases, and the time decreases to ~0.14s.
        PipelinedInfo pi;
        if (!socket->PopPipelinedInfo(&pi)) {
            LOG(WARNING) << "No corresponding PipelinedInfo in socket";
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }

        do {
            InputResponse* msg = static_cast<InputResponse*>(socket->parsing_context());
            if (msg == NULL) {
                msg = new InputResponse;
                socket->reset_parsing_context(msg);
            }

            const int consume_count = (pi.with_auth ? 1 : pi.count);

            ParseError err = msg->response.ConsumePartialIOBuf(*source, consume_count);
            if (err != PARSE_OK) {
                socket->GivebackPipelinedInfo(pi);
                return MakeParseError(err);
            }

            if (pi.with_auth) {
                if (msg->response.reply_size() != 1 ||
                    !(msg->response.reply(0).type() == brpc::REDIS_REPLY_STATUS &&
                      msg->response.reply(0).data().compare("OK") == 0)) {
                    LOG(ERROR) << "Redis Auth failed: " << msg->response;
                    return MakeParseError(PARSE_ERROR_NO_RESOURCE,
                                          "Fail to authenticate with Redis");
                }

                DestroyingPtr<InputResponse> auth_msg(
                     static_cast<InputResponse*>(socket->release_parsing_context()));
                pi.with_auth = false;
                continue;
            }

            CHECK_EQ((uint32_t)msg->response.reply_size(), pi.count);
            msg->id_wait = pi.id_wait;
            socket->release_parsing_context();
            return MakeMessage(msg);
        } while(true);
    }

    return MakeParseError(PARSE_ERROR_ABSOLUTELY_WRONG);
}

void ProcessRedisResponse(InputMessageBase* msg_base) {
    const int64_t start_parse_us = butil::cpuwide_time_us();
    DestroyingPtr<InputResponse> msg(static_cast<InputResponse*>(msg_base));

    const bthread_id_t cid = msg->id_wait;
    Controller* cntl = NULL;
    const int rc = bthread_id_lock(cid, (void**)&cntl);
    if (rc != 0) {
        LOG_IF(ERROR, rc != EINVAL && rc != EPERM)
            << "Fail to lock correlation_id=" << cid << ": " << berror(rc);
        return;
    }

    ControllerPrivateAccessor accessor(cntl);
    Span* span = accessor.span();
    if (span) {
        span->set_base_real_us(msg->base_real_us());
        span->set_received_us(msg->received_us());
        span->set_response_size(msg->response.ByteSize());
        span->set_start_parse_us(start_parse_us);
    }
    const int saved_error = cntl->ErrorCode();
    if (cntl->response() != NULL) {
        if (cntl->response()->GetDescriptor() != RedisResponse::descriptor()) {
            cntl->SetFailed(ERESPONSE, "Must be RedisResponse");
        } else {
            // We work around ParseFrom of pb which is just a placeholder.
            if (msg->response.reply_size() != (int)accessor.pipelined_count()) {
                cntl->SetFailed(ERESPONSE, "pipelined_count=%d of response does "
                                      "not equal request's=%d",
                                      msg->response.reply_size(), accessor.pipelined_count());
            }
            ((RedisResponse*)cntl->response())->Swap(&msg->response);
            if (FLAGS_redis_verbose) {
                LOG(INFO) << "\n[REDIS RESPONSE] "
                          << *((RedisResponse*)cntl->response());
            }
        }
    } // silently ignore the response.

    // Unlocks correlation_id inside. Revert controller's
    // error code if it version check of `cid' fails
    msg.reset();  // optional, just release resourse ASAP
    accessor.OnResponse(cid, saved_error);
}

void ProcessRedisRequest(InputMessageBase* msg_base) { }

void SerializeRedisRequest(butil::IOBuf* buf,
                           Controller* cntl,
                           const google::protobuf::Message* request) {
    if (request == NULL) {
        return cntl->SetFailed(EREQUEST, "request is NULL");
    }
    if (request->GetDescriptor() != RedisRequest::descriptor()) {
        return cntl->SetFailed(EREQUEST, "The request is not a RedisRequest");
    }
    const RedisRequest* rr = (const RedisRequest*)request;
    // We work around SerializeTo of pb which is just a placeholder.
    if (!rr->SerializeTo(buf)) {
        return cntl->SetFailed(EREQUEST, "Fail to serialize RedisRequest");
    }
    ControllerPrivateAccessor(cntl).set_pipelined_count(rr->command_size());
    if (FLAGS_redis_verbose) {
        LOG(INFO) << "\n[REDIS REQUEST] " << *rr;
    }
}

void PackRedisRequest(butil::IOBuf* buf,
                      SocketMessage**,
                      uint64_t /*correlation_id*/,
                      const google::protobuf::MethodDescriptor*,
                      Controller* cntl,
                      const butil::IOBuf& request,
                      const Authenticator* auth) {
    if (auth) {
        std::string auth_str;
        if (auth->GenerateCredential(&auth_str) != 0) {
            return cntl->SetFailed(EREQUEST, "Fail to generate credential");
        }
        buf->append(auth_str);
        ControllerPrivateAccessor(cntl).add_with_auth();
    }

    buf->append(request);
}

const std::string& GetRedisMethodName(
    const google::protobuf::MethodDescriptor*,
    const Controller*) {
    const static std::string REDIS_SERVER_STR = "redis-server";
    return REDIS_SERVER_STR;
}

}  // namespace policy
} // namespace brpc
