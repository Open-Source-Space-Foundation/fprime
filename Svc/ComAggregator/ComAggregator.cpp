// ======================================================================
// \title  ComAggregator.cpp
// \author lestarch
// \brief  cpp file for ComAggregator component implementation class
// ======================================================================

#include "Svc/ComAggregator/ComAggregator.hpp"

namespace Svc {

namespace {
//! Queue slots that must remain free for a timeout signal to be enqueued: the timeout itself plus one
//! in-flight 'fill' and one in-flight 'status' signal (each bounded to one message by the com protocol).
constexpr FwSizeType TIMEOUT_QUEUE_HEADROOM = 3;
}  // namespace

// ----------------------------------------------------------------------
// Component construction and destruction
// ----------------------------------------------------------------------

ComAggregator ::ComAggregator(const char* const compName)
    : ComAggregatorComponentBase(compName),
      m_bufferState(Fw::Buffer::OwnershipState::OWNED),
      m_frameBuffer(m_frameBufferStore, sizeof(m_frameBufferStore)),
      m_frameSerializer(m_frameBuffer.getSerializer()),
      m_allow_timeout(false) {}

ComAggregator ::~ComAggregator() {}

void ComAggregator ::preamble() {
    Fw::Success good = Fw::Success::SUCCESS;
    this->comStatusOut_out(0, good);
}

// ----------------------------------------------------------------------
// Handler implementations for typed input ports
// ----------------------------------------------------------------------

void ComAggregator ::comStatusIn_handler(FwIndexType portNum, Fw::Success& condition) {
    this->aggregationMachine_sendSignal_status(condition);
}

extern "C" void uplink_trace(unsigned char, unsigned short);
void ComAggregator ::dataIn_handler(FwIndexType portNum, Fw::Buffer& data, const ComCfg::FrameContext& context) {
    // ack packets are exactly 12B raw + framing; use size as a cheap ack-vs-other discriminator in the trace
    uplink_trace(20, (unsigned short)data.getSize());
    Svc::ComDataContextPair pair(data, context);
    this->aggregationMachine_sendSignal_fill(pair);
    // PROTOTYPE: FW_PACKET_HAND (uplink ACK) traffic bypasses the FILL-state
    // accumulate-until-full-or-timeout wait. The buffer manager already tags this
    // packet's FrameContext.apid via ComQueue (set from the wire descriptor,
    // ComQueue.cpp:283/301) -- no new port/schema needed. Immediately re-signal
    // 'timeout' right after 'fill': if the fill just transitioned us into FILL
    // state (non-empty), this forces an instant doSend instead of waiting for the
    // next rate-group tick. If the fill instead went to doHold (aggregator was
    // still in WAIT_STATUS on a prior send), this call is a no-op -- WAIT_STATUS
    // has no 'timeout' handler in the state chart, so it's safely dropped; that
    // packet gets flushed as soon as the prior status returns and doClear/doFill
    // replay it, same as it already worked before this change.
    if (context.get_apid() == ComCfg::Apid::FW_PACKET_HAND) {
        uplink_trace(23, 1);
        this->aggregationMachine_sendSignal_timeout();
    }
}

void ComAggregator ::dataReturnIn_handler(FwIndexType portNum, Fw::Buffer& data, const ComCfg::FrameContext& context) {
    FW_ASSERT(this->m_bufferState == Fw::Buffer::OwnershipState::NOT_OWNED);
    this->m_bufferState = Fw::Buffer::OwnershipState::OWNED;
}

void ComAggregator ::timeout_handler(FwIndexType portNum, U32 context) {
    uplink_trace(21, (unsigned short)(this->m_allow_timeout ? 1 : 0));
    // Timeout is ignored in WAIT_STATUS state. However, the queue may not process timeout messages until the wait
    // status is returned because the port chain may be synchronous and downstream components (radio, retry, etc) may
    // take a long time to complete the transmission of data. This can cause the queue to overflow with messages that
    // will soon be discarded.
    //
    // Therefore, to fix the risk of queue overflow we only queue timeout messages when they would be processed by the
    // state machine (i.e. in the FILL state). Otherwise, these messages are not queued.
    //
    // Behaviorally, this solution will work exactly like the naive implementation with an infinite queue depth, but
    // prevents queue overflow when using finite queues.
    //
    // Even so, timeout remains the only signal source without flow control: 'fill' and 'status' are each bounded
    // to one in-flight message by the com protocol, but the rate group keeps delivering ticks while this
    // component's dispatch thread is stalled (downstream backpressure, thread starvation). In the FILL state
    // (m_allow_timeout true) such a stall would still fill the queue with timeout signals and trip the queue-full
    // assertion in the autocoded signal send. Ticks are periodic and idempotent, so additionally skip the signal
    // unless the queue has headroom for it plus the in-flight flow-controlled signals; a skipped tick is simply
    // retried on the next cycle.
    if (this->m_allow_timeout &&
        (this->m_queue.getMessagesAvailable() + TIMEOUT_QUEUE_HEADROOM <= this->m_queue.getDepth())) {
        this->aggregationMachine_sendSignal_timeout();
    }
}

// ----------------------------------------------------------------------
// Implementations for internal state machine actions
// ----------------------------------------------------------------------

void ComAggregator ::Svc_AggregationMachine_action_doClear(SmId smId, Svc_AggregationMachine::Signal signal) {
    this->m_allow_timeout = true;  // Allow timeout messages in FILL state
    this->m_frameSerializer.resetSer();
    this->m_frameBuffer.setSize(sizeof(this->m_frameBufferStore));
    if (this->m_held.get_data().isValid()) {
        // Fill the held data
        this->Svc_AggregationMachine_action_doFill(smId, signal, this->m_held);
        this->m_held = Svc::ComDataContextPair();
    }
}

void ComAggregator ::Svc_AggregationMachine_action_doFill(SmId smId,
                                                          Svc_AggregationMachine::Signal signal,
                                                          const Svc::ComDataContextPair& value) {
    Fw::SerializeStatus status = this->m_frameSerializer.serializeFrom(
        value.get_data().getData(), value.get_data().getSize(), Fw::Serialization::OMIT_LENGTH);
    FW_ASSERT(status == Fw::SerializeStatus::FW_SERIALIZE_OK);
    this->m_lastContext = value.get_context();
    Fw::Success good = Fw::Success::SUCCESS;
    // Return port does not alter data and thus const-cast is safe
    this->dataReturnOut_out(0, const_cast<Fw::Buffer&>(value.get_data()), value.get_context());
    this->comStatusOut_out(0, good);
}

void ComAggregator ::Svc_AggregationMachine_action_doSend(SmId smId, Svc_AggregationMachine::Signal signal) {
    uplink_trace(22, (unsigned short)this->m_frameBuffer.getSize());
    // Send only when the buffer will be valid
    if (this->m_frameSerializer.getSize() > 0) {
        this->m_bufferState = Fw::Buffer::OwnershipState::NOT_OWNED;
        this->m_frameBuffer.setSize(this->m_frameSerializer.getSize());
        this->m_allow_timeout = false;  // Timeout messages should be discarded in WAIT_STATUS state
        this->dataOut_out(0, this->m_frameBuffer, this->m_lastContext);
    }
}

void ComAggregator ::Svc_AggregationMachine_action_doHold(SmId smId,
                                                          Svc_AggregationMachine::Signal signal,
                                                          const Svc::ComDataContextPair& value) {
    FW_ASSERT(not this->m_held.get_data().isValid());
    this->m_held = value;
}

void ComAggregator ::Svc_AggregationMachine_action_assertNoStatus(SmId smId, Svc_AggregationMachine::Signal signal) {
    // Status is not possible in this state, confirm by assertion
    FW_ASSERT(0);
}

// ----------------------------------------------------------------------
// Implementations for internal state machine guards
// ----------------------------------------------------------------------

bool ComAggregator ::Svc_AggregationMachine_guard_isFull(SmId smId,
                                                         Svc_AggregationMachine::Signal signal,
                                                         const Svc::ComDataContextPair& value) const {
    FW_ASSERT(value.get_data().getSize() <= ComCfg::AggregationSize);
    const FwSizeType remaining = this->m_frameSerializer.getCapacity() - this->m_frameSerializer.getSize();
    return (remaining < value.get_data().getSize());
}

bool ComAggregator ::Svc_AggregationMachine_guard_willFill(SmId smId,
                                                           Svc_AggregationMachine::Signal signal,
                                                           const Svc::ComDataContextPair& value) const {
    FW_ASSERT(value.get_data().getSize() <= ComCfg::AggregationSize);
    const FwSizeType remaining = this->m_frameSerializer.getCapacity() - this->m_frameSerializer.getSize();
    return (remaining == value.get_data().getSize());
}

bool ComAggregator ::Svc_AggregationMachine_guard_isNotEmpty(SmId smId, Svc_AggregationMachine::Signal signal) const {
    return this->m_frameSerializer.getSize() > 0;
}

bool ComAggregator ::Svc_AggregationMachine_guard_isGood(SmId smId,
                                                         Svc_AggregationMachine::Signal signal,
                                                         const Fw::Success& value) const {
    return value == Fw::Success::SUCCESS;
}

}  // namespace Svc
