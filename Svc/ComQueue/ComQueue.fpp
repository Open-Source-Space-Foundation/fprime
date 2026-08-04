module Svc {
    @ An enumeration of queue data types
    enum QueueType : U8 { COM_QUEUE, BUFFER_QUEUE }

    @ Array of queue depths for Fw::Com types
    array ComQueueDepth = [ComQueueComPorts] U32

    @ Array of queue depths for Fw::Buffer types
    array BuffQueueDepth = [ComQueueBufferPorts] U32

    @ Component used to queue buffer types
    active component ComQueue {

      # ----------------------------------------------------------------------
      # General ports
      # ----------------------------------------------------------------------

      @ Port for emitting data ready to be sent
      output port dataOut: Svc.ComDataWithContext

      @ Port for receiving the status signal.
      @ `block` (not the default assert) because the com-status protocol is
      @ one-shot: the single arming SUCCESS emitted at startup shares this
      @ message queue with the boot event burst on comPacketQueueIn. If the
      @ queue is momentarily full the assert path drops the status (fatally
      @ wedging the queue in WAITING forever on deployments whose assert hook
      @ does not halt); blocking the (rare, low-rate) status sender until a
      @ slot frees guarantees delivery at zero memory cost.
      async input port comStatusIn: Fw.SuccessCondition block

      @ Port array for receiving Fw::ComBuffers
      async input port comPacketQueueIn: [ComQueueComPorts] Fw.Com drop

      @ Port array for receiving Fw::Buffers
      async input port bufferQueueIn: [ComQueueBufferPorts] Fw.BufferSend hook

      @ Port array for returning ownership of Fw::Buffer to its original sender
      output port bufferReturnOut: [ComQueueBufferPorts] Fw.BufferSend

      @ Port for receiving Fw::Buffer whose ownership needs to be handed back
      sync input port dataReturnIn: Svc.ComDataWithContext

      @ Port for scheduling telemetry output
      async input port run: Svc.Sched drop

      # ----------------------------------------------------------------------
      # Special ports
      # ----------------------------------------------------------------------

      @ Command receive port
      command recv port CmdDisp

      @ Command registration port
      command reg port CmdReg

      @ Command response port
      command resp port CmdStatus

      @ Port for emitting events
      event port Log

      @ Port for emitting text events
      text event port LogText

      @ Port for getting the time
      time get port Time

      @ Port for emitting telemetry
      telemetry port Tlm

      include "ComQueueCommands.fppi"
      include "ComQueueEvents.fppi"
      include "ComQueueTelemetry.fppi"
    }
}
