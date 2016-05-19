// Copyright (c) 2016, Baidu.com, Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <common/string_util.h>
#include <common/logging.h>
#include <common/timer.h>
#include <gflags/gflags.h>

#include "nameserver/sync.h"
#include "rpc/rpc_client.h"

DECLARE_string(nameserver_nodes);
DECLARE_string(nameserver);
DECLARE_string(master_slave_role);

namespace baidu {
namespace bfs {

MasterSlaveImpl::MasterSlaveImpl() : exiting_(false), master_only_(false),
                                     cond_(&mu_),
                                     log_done_(&mu_), read_log_(-1), scan_log_(-1),
                                     current_offset_(0), applied_offset_(0), sync_offset_(0) {
    std::vector<std::string> nodes;
    common::SplitString(FLAGS_nameserver_nodes, ",", &nodes);
    std::string another_server;
    if (FLAGS_nameserver == nodes[0]) {
        another_server = nodes[1];
    } else if (FLAGS_nameserver == nodes[1]) {
        another_server = nodes[0];
    } else {
        LOG(FATAL, "[Sync] Nameserver does not belong to this cluster");
    }
    master_addr_ = FLAGS_master_slave_role == "master" ? FLAGS_nameserver : another_server;
    slave_addr_ = FLAGS_master_slave_role == "slave" ? FLAGS_nameserver : another_server;
    is_leader_ = FLAGS_master_slave_role == "master";
    thread_pool_ = new common::ThreadPool(10);
}

void MasterSlaveImpl::Init() {
    log_ = open("sync.log", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (log_ < 0) {
        LOG(FATAL, "[Sync] open sync log failed reason: %s", strerror(errno));
    }
    current_offset_ = lseek(log_, 0, SEEK_END);
    sync_offset_ = current_offset_;
    LOG(INFO, "[Sync] set current_offset_ to %d", current_offset_);

    read_log_ = open("sync.log", O_RDONLY);
    if (read_log_ < 0)  {
        LOG(FATAL, "[Sync] open sync log for read failed reason: %s", strerror(errno));
    }
    // redo log
    int fp = open("applied.log", O_RDONLY);
    if (fp < 0 && errno != ENOENT) {
        LOG(FATAL, "[Sync] open applied.log failed, reason: %s", strerror(errno));
    }
    if (fp > 0) {
        char buf[4];
        int ret = read(fp, buf, 4);
        if (ret == 4) {
            memcpy(&applied_offset_, buf, 4);
            assert(applied_offset_ <= sync_offset_);
            lseek(read_log_, applied_offset_, SEEK_SET);
        }
        close(fp);
    }
    while (applied_offset_ < sync_offset_) {
        std::string entry;
        if (!ReadEntry(&entry)) {
            assert(0);
        }
        log_callback_(entry);
        applied_offset_ += entry.length() + 4;
    }
    assert(applied_offset_ == sync_offset_);

    rpc_client_ = new RpcClient();
    rpc_client_->GetStub(slave_addr_, &slave_stub_);
    if (IsLeader()) {
        worker_.Start(boost::bind(&MasterSlaveImpl::BackgroundLog, this));
    }
    LogStatus();
}

bool MasterSlaveImpl::IsLeader(std::string* leader_addr) {
    return is_leader_;
}

////// Master //////
bool MasterSlaveImpl::Log(const std::string& entry, int timeout_ms) {
    mu_.Lock();
    int len = LogLocal(entry);
    int last_offset = current_offset_;
    current_offset_ += len;
    cond_.Signal();
    mu_.Unlock();
    // slave is way behind, do no wait
    if (master_only_ && sync_offset_ < last_offset) {
        LOG(WARNING, "[Sync] Sync in maset-only mode, do not wait");
        applied_offset_ = current_offset_;
        return true;
    }

    int64_t start_point = common::timer::get_micros();
    int64_t stop_point = start_point + timeout_ms * 1000;
    while (sync_offset_ != current_offset_ && common::timer::get_micros() < stop_point) {
        MutexLock lock(&mu_);
        int wait_time = (stop_point - common::timer::get_micros()) / 1000;
        if (log_done_.TimeWait(wait_time)) {
            if (sync_offset_ != current_offset_) {
                continue;
            }
            if (master_only_) {
                LOG(INFO, "[Sync] leaves master-only mode");
                master_only_ = false;
            }
            LOG(INFO, "[Sync] sync log takes %ld ms", common::timer::get_micros() - start_point);
            return true;
        } else {
            break;
        }
    }
    // log replicate time out
    LOG(WARNING, "[Sync] Sync log timeout, Sync is in master-only mode");
    master_only_ = true;
    return true;
}

void MasterSlaveImpl::Log(const std::string& entry, boost::function<void (bool)> callback) {
    LOG(INFO, "[Sync] in async log");
    mu_.Lock();
    int len = LogLocal(entry);
    LOG(INFO, "[Sync] log entry len = %d", len);
    if (master_only_ && sync_offset_ < current_offset_) { // slave is behind, do not wait
        callback(true);
        applied_offset_ = current_offset_;
    } else {
        callbacks_.insert(std::make_pair(current_offset_, callback));
        LOG(INFO, "[Sync] insert callback current_offset_ = %d", current_offset_);
        thread_pool_->DelayTask(10000, boost::bind(&MasterSlaveImpl::PorcessCallbck,
                                                   this, current_offset_, entry.length() + 4,
                                                   true));
        cond_.Signal();
    }
    current_offset_ += len;
    mu_.Unlock();
    return;
}

void MasterSlaveImpl::RegisterCallback(boost::function<void (const std::string& log)> callback) {
    log_callback_ = callback;
}

void MasterSlaveImpl::SwitchToLeader() {
    is_leader_ = true;
    sync_offset_ = 0;
    lseek(read_log_, 0, SEEK_SET);
    std::string old_master_addr = master_addr_;
    master_addr_ = slave_addr_;
    slave_addr_ = old_master_addr;
    rpc_client_->GetStub(slave_addr_, &slave_stub_);
    worker_.Start(boost::bind(&MasterSlaveImpl::BackgroundLog, this));
    LOG(INFO, "[Sync] node switch to leader");
}

///    Slave    ///
void MasterSlaveImpl::AppendLog(::google::protobuf::RpcController* controller,
                                const master_slave::AppendLogRequest* request,
                                master_slave::AppendLogResponse* response,
                                ::google::protobuf::Closure* done) {
    if (request->offset() > current_offset_) {
        response->set_offset(current_offset_);
        response->set_success(false);
        done->Run();
        return;
    } else if (request->offset() < current_offset_) {
        LOG(INFO, "[Sync] out-date log request %d, current_offset_ %d",
            request->offset(), current_offset_);
        response->set_offset(-1);
        response->set_success(false);
        done->Run();
        return;
    }
    int len = request->log_data().size();
    char buf[4];
    memcpy(buf, &len, 4);
    write(log_, buf, 4);
    write(log_, request->log_data().c_str(), len);
    log_callback_(request->log_data());
    current_offset_ += len + 4;
    applied_offset_ = current_offset_;
    response->set_success(true);
    done->Run();
}

bool MasterSlaveImpl::ReadEntry(std::string* entry) {
    char buf[4];
    int len;
    int ret = read(read_log_, buf, 4);
    assert(ret == 4);
    memcpy(&len, buf, 4);
    LOG(INFO, "[Sync] record length = %u", len);
    char* tmp = new char[len];
    ret = read(read_log_, tmp, len);
    if (ret == len) {
        entry->assign(tmp, len);
        delete[] tmp;
        return true;
    }
    delete[] tmp;
    return false;
}

void MasterSlaveImpl::BackgroundLog() {
    while (true) {
        MutexLock lock(&mu_);
        while (!exiting_ && sync_offset_ == current_offset_) {
            LOG(INFO, "[Sync] BackgroundLog waiting...");
            cond_.Wait();
        }
        if (exiting_) {
            return;
        }
        LOG(INFO, "[Sync] BackgroundLog logging...");
        mu_.Unlock();
        ReplicateLog();
        mu_.Lock();
    }
}

void MasterSlaveImpl::ReplicateLog() {
    while (sync_offset_ < current_offset_) {
        mu_.Lock();
        if (sync_offset_ == current_offset_) {
            mu_.Unlock();
            break;
        }
        LOG(INFO, "[Sync] ReplicateLog sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
        mu_.Unlock();
        std::string entry;
        if (!ReadEntry(&entry)) {
            LOG(WARNING, "[Sync] incomplete record");
            return;
        }
        master_slave::AppendLogRequest request;
        master_slave::AppendLogResponse response;
        request.set_log_data(entry);
        request.set_offset(sync_offset_);
        while (!rpc_client_->SendRequest(slave_stub_,
                &master_slave::MasterSlave_Stub::AppendLog,
                &request, &response, 15, 1)) {
            LOG(WARNING, "[Sync] Replicate log failed sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
            sleep(5);
        }
        if (!response.success()) { // log mismatch
            MutexLock lock(&mu_);
            if (response.offset() != -1) {
                sync_offset_ = response.offset();
                int offset = lseek(read_log_, sync_offset_, SEEK_SET);
                assert(offset == sync_offset_);
                LOG(INFO, "[Sync] set sync_offset_ to %d", sync_offset_);
            }
            continue;
        }
        PorcessCallbck(sync_offset_, entry.length() + 4, false);
        mu_.Lock();
        sync_offset_ += 4 + entry.length();
        LOG(INFO, "[Sync] Replicate log done. sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
        if (master_only_ && sync_offset_ == current_offset_) {
            master_only_ = false;
            LOG(INFO, "[Sync] leaves master-only mode");
        }
        mu_.Unlock();
    }
    applied_offset_ = current_offset_;
    log_done_.Signal();
}

int MasterSlaveImpl::LogLocal(const std::string& entry) {
    if (!IsLeader()) {
        LOG(FATAL, "[Sync] slave does not need to log");
    }
    int len = entry.length();
    char buf[4];
    memcpy(buf, &len, 4);
    write(log_, buf, 4);
    int w = write(log_, entry.c_str(), entry.length());
    assert(w >= 0);
    return w + 4;
}

void MasterSlaveImpl::PorcessCallbck(int offset, int len, bool timeout_check) {
    boost::function<void (bool)> callback;
    MutexLock lock(&mu_);
    std::map<int, boost::function<void (bool)> >::iterator it = callbacks_.find(offset);
    if (it != callbacks_.end()) {
        callback = it->second;
        callbacks_.erase(it);
        LOG(INFO, "[Sync] calling callback %d", it->first);
        mu_.Unlock();
        callback(true);
        mu_.Lock();
        if (offset + len > applied_offset_) {
            applied_offset_ = offset + len;
        }
        if (timeout_check && !master_only_) {
            LOG(WARNING, "[Sync] ReplicateLog sync_offset_ = %d timeout, enter master-only mode",
                offset);
            master_only_ = true;
            return;
        }
    }
    if (master_only_ && offset + len == current_offset_) {
        LOG(INFO, "[Sync] leaves master-only mode");
        master_only_ = true;
    }
}

void MasterSlaveImpl::LogStatus() {
    LOG(INFO, "[Sync] sync_offset_ = %d, current_offset_ = %d, applied_offset_ = %d, callbacks_ size = %d",
        sync_offset_, current_offset_, applied_offset_, callbacks_.size());
    int fp = open("applied.tmp", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    char buf[4];
    memcpy(buf, &applied_offset_, 4);
    write(fp, buf, 4);
    close(fp);
    rename("applied.tmp", "applied.log");
    thread_pool_->DelayTask(5000, boost::bind(&MasterSlaveImpl::LogStatus, this));
}

} // namespace bfs
} // namespace baidu
