/*
 * Project: curve
 * Created Date: 18-8-23
 * Author: wudemiao
 * Copyright (c) 2018 netease
 */

#include "src/chunkserver/copyset_node.h"

#include <glog/logging.h>

#include <cassert>

#include "src/chunkserver/chunk_closure.h"
#include "src/chunkserver/op_request.h"
#include "src/chunkserver/op_context.h"
#include "src/chunkserver/chunkserverStorage/chunkserver_storage.h"
#include "src/chunkserver/copyset_node_manager.h"

namespace curve {
namespace chunkserver {

CopysetNode::CopysetNode(const LogicPoolID &logicPoolId,
                         const CopysetID &copysetId,
                         const Configuration &initConf,
                         std::unique_ptr<CSDataStore> dataStorePtr) :
    copysetNodeManager_(&CopysetNodeManager::GetInstance()),
    logicPoolId_(logicPoolId),
    copysetId_(copysetId),
    initConf_(initConf),
    peerId_(),
    nodeOptions_(),
    raftNode_(nullptr),
    filesystemProtocol_("local"),
    chunkDataApath_(),
    chunkDataRpath_(),
    dataStore_(std::move(dataStorePtr)),
    leaderTerm_(-1) {}

CopysetNode::~CopysetNode() {
}

int CopysetNode::Init(const CopysetNodeOptions &options) {
    std::string groupId = ToGroupId(logicPoolId_, copysetId_);

    std::string copiedUri(options.chunkDataUri);
    std::string chunkDataDir;
    std::string protocol = FsAdaptorUtil::ParserUri(copiedUri, &chunkDataDir);
    if (protocol.empty()) {
        LOG(ERROR) << "not support chunk data uri's protocol"
                   << "error chunkDataDir is: " << chunkDataDir;
        return -1;
    }

    /**
     * Init copyset node 关于 chunk server 的配置，
     * 这两个的初始化必须在 raftNode_.init 之前
     */
    filesystemProtocol_ = protocol;
    chunkDataApath_.append(chunkDataDir).append("/").append(groupId);
    std::shared_ptr<CSSfsAdaptor> sfsAdaptor =
        ChunkserverStorage::CreateFsAdaptor("", options.chunkDataUri);
    bool ret = dataStore_->Initialize(sfsAdaptor, chunkDataApath_);
    if (true != ret) {
        LOG(ERROR) << "dataStore " << chunkDataApath_ << " init" << "failed";
        return -1;
    }
    chunkDataApath_.append("/data");
    chunkDataRpath_ = "data";
    /* TODO(wudemiao) 后期修改以适应不同的文件系统 */
    fs_ = new PosixFileSystemAdaptor();

    /**
     * Init copyset 对应的 raft node options
     */
    nodeOptions_.initial_conf = initConf_;
    nodeOptions_.election_timeout_ms = options.electionTimeoutMs;
    nodeOptions_.fsm = this;
    nodeOptions_.node_owns_fsm = false;
    nodeOptions_.snapshot_interval_s = options.snapshotIntervalS;
    nodeOptions_.log_uri = options.logUri;
    nodeOptions_.log_uri.append("/").append(groupId).append("/log");
    nodeOptions_.raft_meta_uri = options.raftMetaUri;
    nodeOptions_.raft_meta_uri.append("/").append(groupId).append("/raft_meta");
    nodeOptions_.snapshot_uri = options.raftSnapshotUri;
    nodeOptions_.snapshot_uri.append("/")
        .append(groupId).append("/raft_snapshot");
    nodeOptions_.disable_cli = options.disableCli;
    nodeOptions_.usercode_in_pthread = options.usercodeInPthread;

    /* 初始化 peer id */
    butil::ip_t ip;
    butil::str2ip(options.ip.c_str(), &ip);
    butil::EndPoint addr(ip, options.port);
    /**
     * idx 默认是零，在 chunkserver 不允许一个进程有同一个个 copyset 的多副本，
     * 这一点注意和不让 braft区别开来
     */
    peerId_ = PeerId(addr, 0);
    /* 创建 raft node */
    raftNode_ = std::make_shared<Node>(groupId, peerId_);
    copysetNodeManager_ = options.copysetNodeManager;

    return 0;
}

int CopysetNode::Run() {
    if (0 != raftNode_->init(nodeOptions_)) {
        LOG(ERROR) << "Fail to init raft node "
                   << ToGroupIdString(logicPoolId_, copysetId_);
        return -1;
    }
    return 0;
}

void CopysetNode::Fini() {
    if (nullptr != raftNode_) {
        /* 关闭所有关于此 raft node 的服务 */
        raftNode_->shutdown(nullptr);
        /* 等待所有的正在处理的 task 结束 */
        raftNode_->join();
    }
    if (nullptr != dataStore_) {
        dataStore_->UnInitialize();
    }
}

/* TODO(wudemiao):  Follower read，根据 request 的 committed index 返回读 */
void CopysetNode::ReadChunk(RpcController *controller,
                            const ChunkRequest *request,
                            ChunkResponse *response,
                            Closure *done) {
    ApplyChunkRequest(controller, request, response, done);
}

void CopysetNode::DeleteChunk(RpcController *controller,
                              const ChunkRequest *request,
                              ChunkResponse *response,
                              Closure *done) {
    ApplyChunkRequest(controller, request, response, done);
}

void CopysetNode::WriteChunk(RpcController *controller,
                             const ChunkRequest *request,
                             ChunkResponse *response,
                             Closure *done) {
    ApplyChunkRequest(controller, request, response, done);
}

void CopysetNode::on_apply(::braft::Iterator &iter) {
    for (; iter.valid(); iter.next()) {
        /* 放在 bthread 中异步执行，避免阻塞当前状态机的执行 */
        braft::AsyncClosureGuard doneGuard(iter.done());

        /* 解析 log entry 的 data 部分 */
        butil::IOBuf data = iter.data();
        RequestType type = RequestType::UNKNOWN_OP;
        data.cutn(&type, sizeof(uint8_t));

        ChunkClosure *chunkClosure = nullptr;
        braft::Closure *closure = nullptr;

        switch (type) {
            case RequestType::CHUNK_OP:
                closure = iter.done();
                /**
                 * closure 是 null，那么说明当前节点正常，直接从内存中拿到 Op
                 * context 进行 apply
                 */
                if (nullptr != closure) {
                    chunkClosure = dynamic_cast<ChunkClosure *>(iter.done());
                    assert(nullptr != chunkClosure);
                    ChunkOpContext *opCtx = chunkClosure->GetOpContext();
                    int ret = opCtx->OnApply(shared_from_this());
                    if (0 == ret) {
                        ChunkResponse *response = opCtx->GetResponse();
                        response->set_status(CHUNK_OP_STATUS::CHUNK_OP_STATUS_SUCCESS); //NOLINT
                    } else {
                        LOG(ERROR) << "chunk op apply failed, "
                                   << "errno: " << errno
                                   << " error str: " << strerror(errno);
                    }
                } else {
                    /**
                     * closure 不为 null，说明是节点重启，回放日志 apply，这里会对
                     * Op log entry 进行反序列化，然后获取 Op 信息进行 apply
                     */
                    ChunkOpContext::OnApply(shared_from_this(), &data);
                }
                break;
            default:
                LOG(FATAL) << "Unknown Op type";
                break;
        }
    }
}

void CopysetNode::on_shutdown() {
    LOG(INFO) << ToGroupIdString(logicPoolId_, copysetId_) << ") is shutdown";
}

/* TODO(wudemiao): 快速实现，仅仅实现 data 部分，snapshot 后面再添加 */
void CopysetNode::on_snapshot_save(::braft::SnapshotWriter *writer,
                                   ::braft::Closure *done) {
    brpc::ClosureGuard doneGuard(done);

    std::unique_ptr<DirReader>
        dirReader(fs_->directory_reader(chunkDataApath_));
    if (dirReader->is_valid()) {
        while (dirReader->next()) {
            /* /mnt/sda/1-10001/data/100001.chunk:data/100001.chunk */
            /* 1. 添加绝对路径 */
            std::string filename;
            filename.append(chunkDataApath_);
            filename.append("/").append(dirReader->name());
            /* 2. 添加分隔符 */
            filename.append(":");
            /* 3. 添加相对路径 */
            filename.append(chunkDataRpath_);
            filename.append("/").append(dirReader->name());
            writer->add_file(filename);
        }
    } else {
        done->status().set_error(errno, "invalid: %s", strerror(errno));
        LOG(ERROR) << "dir reader failed, maybe no exist or permission. path "
                   << chunkDataApath_;
    }
}

int CopysetNode::on_snapshot_load(::braft::SnapshotReader *reader) {
    /* 打开的 snapshot path: /mnt/sda/1-10001/raft_snapshot/snapshot_0043 */
    std::string snapshotPath = reader->get_path();

    /* /mnt/sda/1-10001/raft_snapshot/snapshot_0043/data */
    std::string snapshotChunkDataDir;
    snapshotChunkDataDir.append(snapshotPath);
    snapshotChunkDataDir.append("/").append(chunkDataRpath_);

    std::unique_ptr<DirReader>
        dirReader(fs_->directory_reader(snapshotChunkDataDir));
    if (dirReader->is_valid()) {
        while (dirReader->next()) {
            /* /mnt/sda/1-10001/raft_snapshot/snapshot_0043/data/100001.chunk*/
            std::string snapshotFilename;
            snapshotFilename.append(snapshotChunkDataDir).append("/").append(
                dirReader->name());
            /* /mnt/sda/1-10001/data/100001.chunk */
            std::string dataFilename;
            dataFilename.append(chunkDataApath_);
            dataFilename.append("/").append(dirReader->name());
            if (!fs_->rename(snapshotFilename, dataFilename)) {
                LOG(ERROR) << "rename " << snapshotFilename << " to "
                           << dataFilename << " failed";
                return -1;
            }
        }
    } else {
        LOG(ERROR) << "dir reader failed, maybe no exist or permission. path "
                   << snapshotPath;
        return -1;
    }
}

void CopysetNode::on_leader_start(int64_t term) {
    leaderTerm_.store(term, std::memory_order_release);
    LOG(INFO) << ToGroupIdString(logicPoolId_, copysetId_)
              << ", peer id: " << peerId_.to_string()
              << " become leader, term is: " << leaderTerm_;
}

void CopysetNode::on_leader_stop(const butil::Status &status) {
    leaderTerm_.store(-1, std::memory_order_release);
    LOG(INFO) << ToGroupIdString(logicPoolId_, copysetId_)
              << ", peer id: " << peerId_.to_string() << " stepped down";
}

void CopysetNode::on_error(const ::braft::Error &e) {
    LOG(ERROR) << ToGroupIdString(logicPoolId_, copysetId_)
               << ", peer id: " << peerId_.to_string()
               << " meet raft error: " << e;
}

void CopysetNode::on_configuration_committed(const Configuration &conf) {
    LOG(INFO) << "peer id: " << peerId_.to_string()
              << ", Configuration of this group is" << conf;
}

void CopysetNode::on_stop_following(const ::braft::LeaderChangeContext &ctx) {
    LOG(INFO) << ToGroupIdString(logicPoolId_, copysetId_)
              << ", peer id: " << peerId_.to_string()
              << " stops following" << ctx;
}

void CopysetNode::on_start_following(const ::braft::LeaderChangeContext &ctx) {
    LOG(INFO) << ToGroupIdString(logicPoolId_, copysetId_)
              << ", peer id: " << peerId_.to_string()
              << "start following" << ctx;
}

void CopysetNode::RedirectChunkRequest(ChunkResponse *response) {
    PeerId leader = raftNode_->leader_id();
    if (!leader.is_empty()) {
        response->set_redirect(leader.to_string());
    }
    response->set_status(CHUNK_OP_STATUS::CHUNK_OP_STATUS_REDIRECTED);
}

void CopysetNode::ApplyChunkRequest(RpcController *controller,
                                    const ChunkRequest *request,
                                    ChunkResponse *response,
                                    Closure *done) {
    brpc::ClosureGuard doneGuard(done);

    /* 检查任期和自己是不是 Leader */
    const int64_t term = leaderTerm_.load(std::memory_order_acquire);
    int ret = strcmp(peerId_.to_string().c_str(),
                     raftNode_->leader_id().to_string().c_str());
    if (0 > term || 0 != ret) {
        RedirectChunkRequest(response);
        return;
    }
    /* 打包 op 为 task */
    ChunkOpContext *opCtx = new ChunkOpContext(controller,
                                             request,
                                             response,
                                             doneGuard.release());
    braft::Task task;
    butil::IOBuf log;

    if (0 != opCtx->Encode(&log)) {
        /* rpc response 已经在 Encode 内部设置 */
        LOG(ERROR) << "chunk op request encode failure";
        return;
    }
    task.data = &log;
    task.done = new ChunkClosure(this, opCtx);
    /* apply task to raft node process */
    return raftNode_->apply(task);
}

CopysetNodeOptions::CopysetNodeOptions()
    : electionTimeoutMs(1000),
      snapshotIntervalS(3600),
      catchupMargin(1000),
      usercodeInPthread(false),
      disableCli(false),
      logUri("/log"),
      raftMetaUri("/raft_meta"),
      raftSnapshotUri("/raft_snapshot"),
      chunkDataUri("/data"),
      chunkSnapshotUri("/snapshot"),
      port(8200),
      maxChunkSize(16 * 1024 * 1024),
      copysetNodeManager(nullptr) {
}

CopysetNodeOptions &CopysetNodeOptions::operator=(
    const CopysetNodeOptions &copysetNodeOptions) {
    electionTimeoutMs = copysetNodeOptions.electionTimeoutMs;
    snapshotIntervalS = copysetNodeOptions.snapshotIntervalS;
    catchupMargin = copysetNodeOptions.catchupMargin;
    usercodeInPthread = copysetNodeOptions.usercodeInPthread;
    disableCli = copysetNodeOptions.disableCli;
    logUri = copysetNodeOptions.logUri;
    raftMetaUri = copysetNodeOptions.raftMetaUri;
    raftSnapshotUri = copysetNodeOptions.raftSnapshotUri;
    chunkDataUri = copysetNodeOptions.chunkDataUri;
    chunkSnapshotUri = copysetNodeOptions.chunkSnapshotUri;
    ip = copysetNodeOptions.ip;
    port = copysetNodeOptions.port;
    maxChunkSize = copysetNodeOptions.maxChunkSize;
    copysetNodeManager = copysetNodeOptions.copysetNodeManager;
    return *this;
}

}  // namespace chunkserver
}  // namespace curve
