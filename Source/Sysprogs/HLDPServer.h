#pragma once
#include <string>
#include <vector>

class cmCommand;
class BasicIncomingSocket;
struct cmListFileFunction;

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

  typedef int UniqueScopeID;

  class RAIIScope
  {
  private:
    HLDPServer* m_pServer;
    UniqueScopeID m_UniqueID;

  public:
    cmCommand* Command;
    cmMakefile* Makefile;
    const cmListFileFunction& Function;
    std::string SourceFile;
    UniqueScopeID GetUniqueID() { return m_UniqueID; }

  public:
    RAIIScope(HLDPServer* pServer, cmCommand* pCommand, cmMakefile* pMakefile,
              const cmListFileFunction& function);

    RAIIScope(const RAIIScope&) = delete;
    void operator=(const RAIIScope&) = delete;

    ~RAIIScope();
  };

  std::unique_ptr<RAIIScope> OnExecutingInitialPass(
    cmCommand* pCommand, cmMakefile* pMakefile,
    const cmListFileFunction& function);

private:
  bool SendReply(HLDPPacketType packetType, const ReplyBuilder& builder);
  HLDPPacketType ReceiveRequest(
    RequestReader& reader); // Returns 'Invalid' on error
  void SendErrorPacket(std::string details);

private:
  BasicIncomingSocket* m_pSocket;
  bool m_BreakInPending = false;
  bool m_Detached = false;
  std::vector<RAIIScope*> m_CallStack;

  enum
  {
	  kNoScope = -1,
	  kRootScope = -2,
  };

  UniqueScopeID m_NextScopeID = 0;
  UniqueScopeID m_EndOfStepScopeID = kNoScope;
};
}