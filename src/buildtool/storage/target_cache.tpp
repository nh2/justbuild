// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
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

#ifndef INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_TPP
#define INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_TPP

#include "src/buildtool/storage/target_cache.hpp"

template <bool kDoGlobalUplink>
auto TargetCache<kDoGlobalUplink>::Store(
    TargetCacheKey const& key,
    TargetCacheEntry const& value,
    ArtifactDownloader const& downloader) const noexcept -> bool {
    if (not DownloadKnownArtifacts(value, downloader)) {
        return false;
    }
    if (auto digest = cas_->StoreBlob(value.ToJson().dump(2))) {
        auto data =
            Artifact::ObjectInfo{ArtifactDigest{*digest}, ObjectType::File}
                .ToString();
        logger_->Emit(LogLevel::Debug,
                      "Adding entry for key {} as {}",
                      key.Id().ToString(),
                      data);
        return file_store_.AddFromBytes(key.Id().digest.hash(), data);
    }
    return false;
}

template <bool kDoGlobalUplink>
auto TargetCache<kDoGlobalUplink>::Read(
    TargetCacheKey const& key) const noexcept
    -> std::optional<std::pair<TargetCacheEntry, Artifact::ObjectInfo>> {
    auto id = key.Id().digest.hash();
    auto entry_path = file_store_.GetPath(id);

    if constexpr (kDoGlobalUplink) {
        // Uplink any existing target cache entry in storage generations
        [[maybe_unused]] auto found =
            GarbageCollector::GlobalUplinkTargetCacheEntry(key);
    }

    auto const entry =
        FileSystemManager::ReadFile(entry_path, ObjectType::File);
    if (not entry) {
        logger_->Emit(LogLevel::Debug,
                      "Cache miss, entry not found {}",
                      entry_path.string());
        return std::nullopt;
    }
    if (auto info = Artifact::ObjectInfo::FromString(*entry)) {
        if (auto path = cas_->BlobPath(info->digest, /*is_executable=*/false)) {
            if (auto value = FileSystemManager::ReadFile(*path)) {
                try {
                    return std::make_pair(
                        TargetCacheEntry{nlohmann::json::parse(*value)},
                        std::move(*info));
                } catch (std::exception const& ex) {
                    logger_->Emit(LogLevel::Warning,
                                  "Parsing entry for key {} failed with:\n{}",
                                  key.Id().ToString(),
                                  ex.what());
                }
            }
        }
    }
    logger_->Emit(LogLevel::Warning,
                  "Reading entry for key {} failed",
                  key.Id().ToString());
    return std::nullopt;
}

template <bool kDoGlobalUplink>
template <bool kIsLocalGeneration>
requires(kIsLocalGeneration) auto TargetCache<
    kDoGlobalUplink>::LocalUplinkEntry(LocalGenerationTC const& latest,
                                       TargetCacheKey const& key) const noexcept
    -> bool {
    // Determine target cache key path of given generation.
    auto key_digest = key.Id().digest.hash();
    if (FileSystemManager::IsFile(latest.file_store_.GetPath(key_digest))) {
        return true;
    }

    // Determine target cache entry location.
    auto cache_key = file_store_.GetPath(key_digest);
    auto raw_key = FileSystemManager::ReadFile(cache_key);
    if (not raw_key) {
        return false;
    }

    // Determine target cache entry location.
    auto entry_info = Artifact::ObjectInfo::FromString(*raw_key);
    if (not entry_info) {
        return false;
    }

    // Determine target cache entry blob path of given generation.
    auto cache_entry =
        cas_->BlobPath(entry_info->digest, /*is_executable=*/false);
    if (not cache_entry) {
        return false;
    }

    // Determine artifacts referenced by target cache entry.
    auto raw_entry = FileSystemManager::ReadFile(*cache_entry);
    if (not raw_entry) {
        return false;
    }
    nlohmann::json json_desc{};
    try {
        json_desc = nlohmann::json::parse(*raw_entry);
    } catch (std::exception const& ex) {
        return false;
    }
    auto entry = TargetCacheEntry::FromJson(json_desc);
    std::vector<Artifact::ObjectInfo> artifacts_info;
    if (not entry.ToArtifacts(&artifacts_info)) {
        return false;
    }

    // Uplink referenced artifacts.
    for (auto const& info : artifacts_info) {
        if (info.type == ObjectType::Tree) {
            if (not cas_->LocalUplinkTree(*latest.cas_, info.digest)) {
                return false;
            }
        }
        else if (not cas_->LocalUplinkBlob(*latest.cas_,
                                           info.digest,
                                           IsExecutableObject(info.type))) {
            return false;
        }
    }

    // Uplink target cache entry blob.
    if (not cas_->LocalUplinkBlob(*latest.cas_,
                                  entry_info->digest,
                                  /*is_executable=*/false)) {
        return false;
    }

    // Uplink target cache key
    return latest.file_store_.AddFromFile(
        key_digest, cache_key, /*is_owner=*/true);
}

template <bool kDoGlobalUplink>
auto TargetCache<kDoGlobalUplink>::DownloadKnownArtifacts(
    TargetCacheEntry const& value,
    ArtifactDownloader const& downloader) const noexcept -> bool {
    std::vector<Artifact::ObjectInfo> artifacts_info;
    return downloader and value.ToArtifacts(&artifacts_info) and
           downloader(artifacts_info);
}

#endif  // INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_TPP
