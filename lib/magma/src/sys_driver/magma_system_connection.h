// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _MAGMA_SYSTEM_CONNECTION_H_
#define _MAGMA_SYSTEM_CONNECTION_H_

#include "magma_system_buffer.h"
#include "magma_system_context.h"
#include "magma_util/macros.h"
#include "magma_util/platform/platform_connection.h"
#include "msd.h"

#include <memory>
#include <unordered_map>

using msd_connection_unique_ptr_t =
    std::unique_ptr<msd_connection, decltype(&msd_connection_close)>;

static inline msd_connection_unique_ptr_t MsdConnectionUniquePtr(msd_connection* conn)
{
    return msd_connection_unique_ptr_t(conn, &msd_connection_close);
}

class MagmaSystemDevice;

class MagmaSystemConnection : private MagmaSystemContext::Owner,
                              public magma::PlatformConnection::Delegate {
public:
    MagmaSystemConnection(std::weak_ptr<MagmaSystemDevice> device,
                          msd_connection_unique_ptr_t msd_connection, uint32_t capabilities);

    ~MagmaSystemConnection();

    // Create a buffer from the handle and add it to the map,
    // on success |id_out| contains the id to be used to query the map
    bool ImportBuffer(uint32_t handle, uint64_t* id_out) override;
    // This removes the reference to the shared_ptr in the map
    // other instances remain valid until deleted
    // Returns false if no buffer with the given |id| exists in the map
    bool ReleaseBuffer(uint64_t id) override;
    // Attempts to locate a buffer by |id| in the buffer map and return it.
    // Returns nullptr if the buffer is not found
    std::shared_ptr<MagmaSystemBuffer> LookupBuffer(uint64_t id);

    bool ExecuteCommandBuffer(uint64_t command_buffer_id, uint32_t context_id) override;

    bool CreateContext(uint32_t context_id) override;
    bool DestroyContext(uint32_t context_id) override;
    MagmaSystemContext* LookupContext(uint32_t context_id);

    bool WaitRendering(uint64_t buffer_id) override;

    uint32_t GetDeviceId();

    msd_connection* msd_connection() { return msd_connection_.get(); }

    void PageFlip(uint64_t id, magma_system_pageflip_callback_t callback, void* data) override;

    std::shared_ptr<magma::PlatformEvent> ShutdownEvent() override { return shutdown_event_; }

    void SignalShutdown() { shutdown_event_->Signal(); }

private:
    std::weak_ptr<MagmaSystemDevice> device_;
    msd_connection_unique_ptr_t msd_connection_;
    std::unordered_map<uint32_t, std::unique_ptr<MagmaSystemContext>> context_map_;
    std::unordered_map<uint64_t, std::shared_ptr<MagmaSystemBuffer>> buffer_map_;
    bool has_display_capability_;
    bool has_render_capability_;
    std::shared_ptr<magma::PlatformEvent> shutdown_event_;

    // MagmaSystemContext::Owner
    std::shared_ptr<MagmaSystemBuffer> LookupBufferForContext(uint64_t id) override
    {
        return LookupBuffer(id);
    }
};

#endif //_MAGMA_SYSTEM_CONNECTION_H_
