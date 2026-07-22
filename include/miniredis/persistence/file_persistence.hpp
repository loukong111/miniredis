#pragma once

#include "miniredis/core/snapshot.hpp"
#include <string>

namespace miniredis {

class FilePersistence {
public:
    explicit FilePersistence(const std::string& filepath);
    bool saveSnapshot(const SnapshotData& data);
    bool loadSnapshot(SnapshotData& out);

private:
    std::string filepath_;
};

} // namespace miniredis
