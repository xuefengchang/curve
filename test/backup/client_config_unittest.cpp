/*
 * Project: curve
 * File Created: Monday, 24th December 2018 5:52:46 pm
 * Author: tongguangxun
 * Copyright (c)￼ 2018 netease
 */

#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string>
#include "src/client/client_config.h"
#include "include/client/libcurve_qemu.h"

extern std::string configpath;   // NOLINT

using curve::client::ClientConfig;

TEST(ClientConfig, ClientConfigTest) {
    ASSERT_EQ(0, Init(configpath.c_str()));

    auto metaopt = ClientConfig::GetMetaServerOption();
    auto ctxslab = ClientConfig::GetContextSlabOption();
    auto reqschd = ClientConfig::GetRequestSchedulerOption();
    auto failreq = ClientConfig::GetFailureRequestOption();
    auto metacah = ClientConfig::GetMetaCacheOption();
    auto iooptio = ClientConfig::GetIOOption();

    ASSERT_STREQ(metaopt.metaaddr.c_str(), "127.0.0.1:8000");
    ASSERT_EQ(ctxslab.pre_allocate_context_num, 1024);
    ASSERT_EQ(reqschd.queueCapacity, 4096);
    ASSERT_EQ(reqschd.threadpoolSize, 2);
    ASSERT_EQ(failreq.opMaxRetry, 3);
    ASSERT_EQ(failreq.opRetryIntervalUs, 200000);
    ASSERT_EQ(metacah.getLeaderRetry, 3);
    ASSERT_EQ(iooptio.enableAppliedIndexRead, 1);
}
