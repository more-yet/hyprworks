#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace hyprworks {

template<class Snapshot>
void publish_snapshot(std::shared_ptr<const Snapshot>& published, Snapshot& candidate) {
    candidate.revision = published ? published->revision : 0;
    if (!published || candidate != *published) {
        if (candidate.revision == UINT64_MAX) throw std::runtime_error("snapshot revisions exhausted");
        ++candidate.revision;
        published = std::make_shared<const Snapshot>(std::move(candidate));
    }
    candidate = {};
}

}
