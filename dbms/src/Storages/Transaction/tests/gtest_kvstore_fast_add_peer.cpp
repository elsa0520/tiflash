// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Debug/MockRaftStoreProxy.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/Page/V3/Universal/UniversalPageStorageService.h>
#include <Storages/S3/CheckpointManifestS3Set.h>
#include <Storages/S3/S3Common.h>
#include <Storages/Transaction/FastAddPeer.h>
#include <Storages/Transaction/FastAddPeerAsyncTasksImpl.h>
#include <Storages/Transaction/ProxyFFI.h>
#include <Storages/Transaction/tests/kvstore_helper.h>
#include <TestUtils/TiFlashTestEnv.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <common/logger_useful.h>

#include <chrono>
#include <numeric>
#include <optional>
#include <thread>

using raft_serverpb::RaftApplyState;
using raft_serverpb::RegionLocalState;

namespace DB
{
FastAddPeerRes genFastAddPeerRes(FastAddPeerStatus status, std::string && apply_str, std::string && region_str);
TempUniversalPageStoragePtr reuseOrCreateTempPageStorage(Context & context, const String & manifest_key);

namespace tests
{
class RegionKVStoreTestFAP : public RegionKVStoreTest
{
public:
    void SetUp() override
    {
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        if (global_context.getSharedContextDisagg()->remote_data_store == nullptr)
        {
            already_initialize_data_store = false;
            global_context.getSharedContextDisagg()->initRemoteDataStore(global_context.getFileProvider(), /*s3_enabled*/ true);
            ASSERT_TRUE(global_context.getSharedContextDisagg()->remote_data_store != nullptr);
        }
        else
        {
            already_initialize_data_store = true;
        }
        orig_mode = global_context.getPageStorageRunMode();
        global_context.setPageStorageRunMode(PageStorageRunMode::UNI_PS);
        global_context.getSharedContextDisagg()->initFastAddPeerContext();
        RegionKVStoreTest::SetUp();
    }

    void TearDown() override
    {
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        if (!already_initialize_data_store)
        {
            global_context.getSharedContextDisagg()->remote_data_store = nullptr;
        }
        global_context.setPageStorageRunMode(orig_mode);
    }

protected:
    bool createBucketIfNotExist()
    {
        auto s3_client = S3::ClientFactory::instance().sharedClient();
        auto bucket = S3::ClientFactory::instance().bucket();
        Aws::S3::Model::CreateBucketRequest request;
        request.SetBucket(bucket);
        auto outcome = s3_client->CreateBucket(request);
        if (outcome.IsSuccess())
        {
            LOG_DEBUG(Logger::get(), "Created bucket {}", bucket);
        }
        else if (outcome.GetError().GetExceptionName() == "BucketAlreadyOwnedByYou")
        {
            LOG_DEBUG(Logger::get(), "Bucket {} already exist", bucket);
        }
        else
        {
            const auto & err = outcome.GetError();
            LOG_ERROR(Logger::get(), "CreateBucket: {}:{}", err.GetExceptionName(), err.GetMessage());
        }
        return outcome.IsSuccess() || outcome.GetError().GetExceptionName() == "BucketAlreadyOwnedByYou";
    }

    void dumpCheckpoint()
    {
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        auto temp_dir = TiFlashTestEnv::getTemporaryPath() + "/";
        auto page_storage = global_context.getWriteNodePageStorage();
        KVStore & kvs = getKVS();
        auto store_id = kvs.getStore().store_id.load();
        auto wi = PS::V3::CheckpointProto::WriterInfo();
        {
            wi.set_store_id(store_id);
        }

        auto remote_store = global_context.getSharedContextDisagg()->remote_data_store;
        assert(remote_store != nullptr);
        UniversalPageStorage::DumpCheckpointOptions opts{
            .data_file_id_pattern = S3::S3Filename::newCheckpointDataNameTemplate(store_id, upload_sequence),
            .data_file_path_pattern = temp_dir + "dat_{seq}_{index}",
            .manifest_file_id_pattern = S3::S3Filename::newCheckpointManifestNameTemplate(store_id),
            .manifest_file_path_pattern = temp_dir + "mf_{seq}",
            .writer_info = wi,
            .must_locked_files = {},
            .persist_checkpoint = CheckpointUploadFunctor{
                .store_id = store_id,
                // Note that we use `upload_sequence` but not `snapshot.sequence` for
                // the S3 key.
                .sequence = upload_sequence,
                .remote_store = remote_store,
            },
            .override_sequence = upload_sequence, // override by upload_sequence
        };
        page_storage->dumpIncrementalCheckpoint(opts);
    }

protected:
    UInt64 upload_sequence = 1000;

private:
    ContextPtr context;
    bool already_initialize_data_store = false;
    DB::PageStorageRunMode orig_mode;
};

TEST_F(RegionKVStoreTestFAP, FAPThreadPool)
try
{
    auto * log = &Poco::Logger::get("RegionKVStoreTest");
    using namespace std::chrono_literals;
    auto fap_context = std::make_shared<FastAddPeerContext>(1);
    auto async_tasks = fap_context->tasks_trace;

    int total = 5;
    std::vector<bool> f(total, false);
    while (true)
    {
        auto count = std::accumulate(f.begin(), f.end(), 0, [&](int a, bool b) -> int {
            return a + int(b);
        });
        if (count >= total)
        {
            break;
        }
        else
        {
            LOG_DEBUG(log, "finished {}/{}", count, total);
        }
        for (int i = 0; i < total; i++)
        {
            if (!async_tasks->isScheduled(i))
            {
                auto res = async_tasks->addTask(i, []() {
                    std::this_thread::sleep_for(1000ms);
                    return genFastAddPeerRes(FastAddPeerStatus::WaitForData, "", "");
                });
                UNUSED(res);
            }
        }

        for (int i = 0; i < total; i++)
        {
            if (!f[i])
            {
                if (async_tasks->isReady(i))
                {
                    auto r = async_tasks->fetchResult(i);
                    UNUSED(r);
                    f[i] = true;
                }
            }
        }
        std::this_thread::sleep_for(1000ms);
    }
}
CATCH

void persistAfterWrite(Context & ctx, KVStore & kvs, std::unique_ptr<MockRaftStoreProxy> & proxy_instance, UniversalPageStoragePtr page_storage, uint64_t region_id, uint64_t index)
{
    MockRaftStoreProxy::FailCond cond;
    proxy_instance->doApply(kvs, ctx.getTMTContext(), cond, region_id, index);
    auto region = proxy_instance->getRegion(region_id);
    auto wb = region->persistMeta();
    page_storage->write(std::move(wb), nullptr);
    // There shall be data to flush.
    ASSERT_EQ(kvs.needFlushRegionData(region_id, ctx.getTMTContext()), true);
    ASSERT_EQ(kvs.tryFlushRegionData(region_id, false, false, ctx.getTMTContext(), 0, 0), true);
}

TEST_F(RegionKVStoreTestFAP, RestoreRaftState)
try
{
    auto & global_context = TiFlashTestEnv::getGlobalContext();
    uint64_t region_id = 1;
    auto peer_id = 1;
    KVStore & kvs = getKVS();
    auto page_storage = global_context.getWriteNodePageStorage();

    proxy_instance->bootstrap(kvs, global_context.getTMTContext(), region_id);
    auto region = proxy_instance->getRegion(region_id);
    auto store_id = kvs.getStore().store_id.load();
    region->addPeer(store_id, peer_id, metapb::PeerRole::Learner);

    // Write some data, and persist meta.
    auto [index, term] = proxy_instance->normalWrite(region_id, {34}, {"v2"}, {WriteCmdType::Put}, {ColumnFamilyType::Default});
    persistAfterWrite(global_context, kvs, proxy_instance, page_storage, region_id, index);

    ASSERT_TRUE(createBucketIfNotExist());
    dumpCheckpoint();

    std::optional<CheckpointInfoPtr> checkpoint_info;
    auto s3_client = S3::ClientFactory::instance().sharedClient();
    auto bucket = S3::ClientFactory::instance().bucket();
    const auto manifests = S3::CheckpointManifestS3Set::getFromS3(*s3_client, bucket, store_id);
    ASSERT_TRUE(!manifests.empty());
    const auto & latest_manifest_key = manifests.latestManifestKey();
    auto temp_ps_wrapper = reuseOrCreateTempPageStorage(global_context, latest_manifest_key);

    RaftApplyState apply_state;
    {
        auto apply_state_key = UniversalPageIdFormat::toRaftApplyStateKeyInKVEngine(region_id);
        auto page = temp_ps_wrapper->temp_ps->read(apply_state_key);
        apply_state.ParseFromArray(page.data.begin(), page.data.size());
    }

    RegionLocalState region_state;
    {
        auto local_state_key = UniversalPageIdFormat::toRegionLocalStateKeyInKVEngine(region_id);
        auto page = temp_ps_wrapper->temp_ps->read(local_state_key);
        region_state.ParseFromArray(page.data.begin(), page.data.size());
    }

    ASSERT_TRUE(apply_state == region->getApply());
    ASSERT_TRUE(region_state == region->getState());

    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    ASSERT_TRUE(fap_context->getTempUniversalPageStorage(store_id, upload_sequence) != nullptr);
    ASSERT_TRUE(fap_context->getTempUniversalPageStorage(store_id, upload_sequence - 1) != nullptr);
    ASSERT_TRUE(fap_context->getTempUniversalPageStorage(store_id, upload_sequence + 1) == nullptr);
}
CATCH
} // namespace tests
} // namespace DB