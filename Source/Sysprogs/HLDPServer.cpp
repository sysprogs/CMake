#include "cmCommand.h"
#include "cmMakefile.h"
///
#include "BasicIncomingSocket.h"
#include "HLDP.h"
#include "HLDPServer.h"
#include "cmSystemTools.h"

namespace Sysprogs {
class ReplyBuilder
{
private:
  std::vector<char> m_Reply;

public:
  ReplyBuilder() { m_Reply.reserve(128); }

  void AppendData(const void* pData, size_t size)
  {
    size_t offset = m_Reply.size();
    m_Reply.resize(offset + size);
    memcpy(m_Reply.data() + offset, pData, size);
  }

  void AppendInt32(int value)
  {
    static_assert(sizeof(value) == 4, "Unexpected sizeof(int)");
    AppendData(&value, sizeof(value));
  }

  void Reset() { m_Reply.resize(0); }

  class DelayedSlot
  {
  private:
    ReplyBuilder& m_Builder;
    size_t m_Offset;

  public:
    DelayedSlot(ReplyBuilder& builder, size_t offset)
      : m_Builder(builder)
      , m_Offset(offset)
    {
    }

    unsigned& operator*()
    {
      return *(unsigned*)(m_Builder.m_Reply.data() + m_Offset);
    }
  };

  DelayedSlot AppendDelayedInt32(unsigned initialValue = 0)
  {
    DelayedSlot slot(*this, m_Reply.size());
    AppendInt32(initialValue);
    return std::move(slot);
  }

  void AppendString(const char* pString)
  {
    if (!pString)
      pString = "";
    int len = strlen(pString);
    AppendInt32(len);
    AppendData(pString, len);
  }

  void AppendString(const std::string& string)
  {
    int len = string.size();
    AppendInt32(len);
    AppendData(string.c_str(), len);
  }

  const std::vector<char>& GetBuffer() const { return m_Reply; }
};

class RequestReader
{
private:
  std::vector<char> m_Request;
  int m_ReadPosition;

public:
  void* Reset(size_t payloadSize)
  {
    m_Request.resize(payloadSize);
    m_ReadPosition = 0;
    return m_Request.data();
  }
};

HLDPServer::HLDPServer(int tcpPort)
  : m_BreakInPending(true)
{
  m_pSocket = new BasicIncomingSocket(tcpPort);
}

HLDPServer::~HLDPServer()
{
  delete m_pSocket;
}

bool HLDPServer::WaitForClient()
{
  if (!m_pSocket->Accept())
    return false;

  m_pSocket->Write(HLDPBanner, sizeof(HLDPBanner));
  ReplyBuilder builder;
  builder.AppendInt32(HLDPVersion);
  builder.AppendString("$->");
  if (!SendReply(HLDPPacketType::scHandshake, builder))
    return false;

  RequestReader reader;

  if (ReceiveRequest(reader) != HLDPPacketType::csHandshake) {
    cmSystemTools::Error("Failed to complete HLDP handshake.");
    return false;
  }

  return true;
}

void HLDPServer::OnExecutingInitialPass(cmCommand* pCommand,
                                        const std::vector<std::string>& args)
{
  if (!m_BreakInPending) {
    if (m_pSocket->HasIncomingData()) {
      RequestReader reader;
      HLDPPacketType requestType = ReceiveRequest(reader);
      switch (requestType) {
        case HLDPPacketType::Invalid:
          return;
        case HLDPPacketType::csBreakIn:
          m_BreakInPending = true;
          break;
        default:
          SendErrorPacket(
            "Unexpected packet received while the target is running");
          return;
      }
    }

    return;
  }

  m_BreakInPending = false;
  ReplyBuilder builder;
  builder.AppendInt32((unsigned)TargetStopReason::InitialBreakIn);
  builder.AppendInt32(0);
  builder.AppendString("");

  cmListFileBacktrace backtraceCopy = pCommand->GetMakefile()->GetBacktrace();
  auto backtraceEntryCount = builder.AppendDelayedInt32();

  int frameNumber = 0;

  while (!backtraceCopy.Top().FilePath.empty()) {
    builder.AppendInt32(frameNumber);
    builder.AppendString(backtraceCopy.Top().Name);
    builder.AppendString(""); // TODO: format arguments if known
    builder.AppendString(backtraceCopy.Top().FilePath);
    builder.AppendInt32(backtraceCopy.Top().Line);

    backtraceCopy = backtraceCopy.Pop();
    (*backtraceEntryCount)++;
  }

  if (!SendReply(HLDPPacketType::scTargetStopped, builder))
    return;

  for (;;) {
    builder.Reset();

    RequestReader reader;
    HLDPPacketType requestType = ReceiveRequest(reader);
    switch (requestType) {
        // TODO: resend backtrace.
      case HLDPPacketType::csBreakIn:
        continue; // The target is already stopped.
      case HLDPPacketType::csContinue:
      case HLDPPacketType::csStepIn:
      case HLDPPacketType::csStepOut:
      case HLDPPacketType::csStepOver:
        m_BreakInPending = true;
        SendReply(HLDPPacketType::scTargetRunning, builder);
        return;
      default:
        SendErrorPacket(
          "Unexpected packet received while the target is stopped");
        break;
    }
  }
}

bool HLDPServer::SendReply(HLDPPacketType packetType,
                           const ReplyBuilder& builder)
{
  HLDPPacketHeader hdr = { (unsigned)packetType, builder.GetBuffer().size() };

  if (!m_pSocket->Write(&hdr, sizeof(hdr))) {
    cmSystemTools::Error("Failed to write debug protocol reply header.");
    cmSystemTools::SetFatalErrorOccured();
    return false;
  }

  if (!m_pSocket->Write(builder.GetBuffer().data(), hdr.PayloadSize)) {
    cmSystemTools::Error("Failed to write debug protocol reply payload.");
    cmSystemTools::SetFatalErrorOccured();
    return false;
  }

  return true;
}

HLDPPacketType HLDPServer::ReceiveRequest(RequestReader& reader)
{
  HLDPPacketHeader hdr;
  if (!m_pSocket->ReadAll(&hdr, sizeof(hdr))) {
    cmSystemTools::Error("Failed to receive debug protocol request header.");
    cmSystemTools::SetFatalErrorOccured();
    return HLDPPacketType::Invalid;
  }

  void* pBuffer = reader.Reset(hdr.PayloadSize);
  if (hdr.PayloadSize != 0) {
    if (!m_pSocket->ReadAll(pBuffer, hdr.PayloadSize)) {
      cmSystemTools::Error(
        "Failed to receive debug protocol request payload.");
      cmSystemTools::SetFatalErrorOccured();
      return HLDPPacketType::Invalid;
    }
  }

  return (HLDPPacketType)hdr.Type;
}

void HLDPServer::SendErrorPacket(std::string details)
{
  ReplyBuilder builder;
  builder.AppendString(details);
  SendReply(HLDPPacketType::scError, builder);
}
}