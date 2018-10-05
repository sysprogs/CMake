#pragma once
#include <string>
#include <vector>

class cmCommand;
class BasicIncomingSocket;

namespace Sysprogs {
class ReplyBuilder;
class RequestReader;
enum class HLDPPacketType;

class HLDPServer
{
public:
  HLDPServer(int tcpPort);
  ~HLDPServer();

  bool WaitForClient();
  void OnExecutingInitialPass(cmCommand* pCommand,
                              const std::vector<std::string>& args);

private:
  bool SendReply(HLDPPacketType packetType, const ReplyBuilder& builder);
  HLDPPacketType ReceiveRequest(
    RequestReader& reader); // Returns 'Invalid' on error
  void SendErrorPacket(std::string details);

private:
  BasicIncomingSocket* m_pSocket;
  bool m_BreakInPending;
};
}