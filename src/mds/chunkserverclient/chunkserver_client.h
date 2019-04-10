/*
 * Project: curve
 * Created Date: Fri Mar 08 2019
 * Author: xuchaojie
 * Copyright (c) 2018 netease
 */

#ifndef SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVER_CLIENT_H_
#define SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVER_CLIENT_H_

#include <brpc/channel.h>
#include <butil/endpoint.h>

#include <string>

#include "src/mds/common/mds_define.h"
#include "src/mds/topology/topology_manager.h"
#include "proto/cli.pb.h"
#include "proto/chunk.pb.h"

using ::curve::mds::topology::Topology;
using ::curve::mds::topology::TopologyManager;
using ::curve::mds::topology::ChunkServerIdType;

namespace curve {
namespace mds {
namespace chunkserverclient {

class ChunkServerClient {
 public:
    explicit ChunkServerClient(std::shared_ptr<Topology> topology)
        :topology_(topology) {}
    virtual ~ChunkServerClient() {}

    /**
     * @brief 删除快照chunk文件
     *
     * @param leaderId leader的ID
     * @param logicalPoolId 逻辑池的ID
     * @param copysetId 复制组的ID
     * @param chunkId chunk文件ID
     * @param sn 文件版本号
     *
     * @return 错误码
     */
    virtual int DeleteChunkSnapshot(ChunkServerIdType leaderId,
        LogicalPoolID logicalPoolId,
        CopysetID copysetId,
        ChunkID chunkId,
        uint64_t sn);

    /**
     * @brief 获取leader
     * @detail
     *   向目标chunkserver发送报文查询leader
     *
     * @param csId 目标chunkserverId
     * @param logicalPoolId 逻辑池ID
     * @param copysetId 复制组的ID
     * @param[out] leader 当前leader
     *
     * @return 错误码
     */
    virtual int GetLeader(ChunkServerIdType csId,
        LogicalPoolID logicalPoolId,
        CopysetID copysetId,
        ChunkServerIdType * leader);

 private:
    std::shared_ptr<Topology> topology_;
};

}  // namespace chunkserverclient
}  // namespace mds
}  // namespace curve


#endif  // SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVER_CLIENT_H_
