// ======================================================================
// \title  File.cpp
// \author bocchino
// \brief  cpp file for FileUplink::File
//
// \copyright
// Copyright 2009-2016, by the California Institute of Technology.
// ALL RIGHTS RESERVED.  United States Government Sponsorship
// acknowledged.
//
// ======================================================================

#include <Fw/Types/Assert.hpp>
#include <Fw/Types/StringUtils.hpp>
#include <Svc/FileUplink/FileUplink.hpp>

#include <cstring>
namespace Svc {

Os::File::Status FileUplink::File::open(const Fw::FilePacket::StartPacket& startPacket) {
    const U32 length = startPacket.getDestinationPath().getLength();
    char path[Fw::FilePacket::PathName::MAX_LENGTH + 1];
    memcpy(path, startPacket.getDestinationPath().getValue(), length);
    path[length] = 0;
    Fw::LogStringArg logStringArg(path);
    this->name = logStringArg;
    this->size = startPacket.getFileSize();
    CFDP::Checksum checksum;
    this->m_checksum = checksum;
    this->m_stageBase = 0;
    this->m_stageLen = 0;
    return this->osFile.open(path, Os::File::OPEN_WRITE);
}

Os::File::Status FileUplink::File::write(const U8* const data, const U32 byteOffset, const U32 length) {
    // PROTOTYPE (uplink throughput): stage sequential payloads in RAM and
    // write the SD-backed file in STAGE_SIZE extents. See FileUplink.hpp.
    Os::File::Status status;

    // Non-sequential offset: flush what we have before handling this packet
    if ((this->m_stageLen > 0) && (byteOffset != this->m_stageBase + this->m_stageLen)) {
        status = this->flush();
        if (status != Os::File::OP_OK) {
            return status;
        }
    }

    if (length > STAGE_SIZE) {
        // Oversized write: flush any staged bytes, then write directly
        status = this->flush();
        if (status != Os::File::OP_OK) {
            return status;
        }
        status = this->osFile.seek(byteOffset, Os::File::SeekType::ABSOLUTE);
        if (status != Os::File::OP_OK) {
            return status;
        }
        FwSizeType intLength = length;
        // Note: not waiting for the file write to finish
        status = this->osFile.write(data, intLength, Os::File::WaitType::NO_WAIT);
        if (status != Os::File::OP_OK) {
            return status;
        }
        FW_ASSERT(static_cast<U32>(intLength) == length, static_cast<FwAssertArgType>(intLength));
    } else {
        // Staged path: make room, then append to the RAM buffer
        if (this->m_stageLen + length > STAGE_SIZE) {
            status = this->flush();
            if (status != Os::File::OP_OK) {
                return status;
            }
        }
        if (this->m_stageLen == 0) {
            this->m_stageBase = byteOffset;
        }
        (void)memcpy(&this->m_stage[this->m_stageLen], data, length);
        this->m_stageLen += length;
    }

    this->m_checksum.update(data, byteOffset, length);
    return Os::File::OP_OK;
}

Os::File::Status FileUplink::File::flush() {
    if (this->m_stageLen == 0) {
        return Os::File::OP_OK;
    }
    Os::File::Status status = this->osFile.seek(this->m_stageBase, Os::File::SeekType::ABSOLUTE);
    if (status != Os::File::OP_OK) {
        return status;
    }
    FwSizeType intLength = this->m_stageLen;
    status = this->osFile.write(this->m_stage, intLength, Os::File::WaitType::NO_WAIT);
    if (status != Os::File::OP_OK) {
        return status;
    }
    FW_ASSERT(static_cast<U32>(intLength) == this->m_stageLen, static_cast<FwAssertArgType>(intLength));
    this->m_stageBase += this->m_stageLen;
    this->m_stageLen = 0;
    return Os::File::OP_OK;
}

}  // namespace Svc
