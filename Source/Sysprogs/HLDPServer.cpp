#include "cmCommand.h"
#include "cmMakefile.h"
#include "BasicIncomingSocket.h"
#include "HLDP.h"
#include "HLDPServer.h"
#include "cmSystemTools.h"
#include "cmStatePrivate.h"

namespace Sysprogs
{
	class ReplyBuilder
	{
	private:
		std::vector<char> m_Reply;

	public:
		ReplyBuilder() { m_Reply.reserve(128); }

		void AppendData(const void *pData, size_t size)
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
			ReplyBuilder &m_Builder;
			size_t m_Offset;

		public:
			DelayedSlot(ReplyBuilder &builder, size_t offset) : m_Builder(builder), m_Offset(offset) {}

			unsigned &operator*() { return *(unsigned *)(m_Builder.m_Reply.data() + m_Offset); }
		};

		DelayedSlot AppendDelayedInt32(unsigned initialValue = 0)
		{
			DelayedSlot slot(*this, m_Reply.size());
			AppendInt32(initialValue);
			return std::move(slot);
		}

		void AppendString(const char *pString)
		{
			if (!pString)
				pString = "";
			int len = strlen(pString);
			AppendInt32(len);
			AppendData(pString, len);
		}

		void AppendString(const std::string &string)
		{
			int len = string.size();
			AppendInt32(len);
			AppendData(string.c_str(), len);
		}

		const std::vector<char> &GetBuffer() const { return m_Reply; }
	};

	class RequestReader
	{
	private:
		std::vector<char> m_Request;
		int m_ReadPosition;

	public:
		void *Reset(size_t payloadSize)
		{
			m_Request.resize(payloadSize);
			m_ReadPosition = 0;
			return m_Request.data();
		}

		bool ReadInt32(int *pValue)
		{
			static_assert(sizeof(*pValue) == 4, "Unexpected sizeof(int)");

			if ((m_ReadPosition + sizeof(*pValue)) > m_Request.size())
				return false;
			memcpy(pValue, m_Request.data() + m_ReadPosition, sizeof(*pValue));
			m_ReadPosition += sizeof(*pValue);
			return true;
		}

		bool ReadString(std::string *pStr)
		{
			int size;
			if (!ReadInt32(&size))
				return false;
			if ((m_ReadPosition + size) > m_Request.size())
				return false;

			*pStr = std::string(m_Request.data() + m_ReadPosition, m_Request.data() + m_ReadPosition + size);
			m_ReadPosition += size;
			return true;
		}
	};

	class HLDPServer::SimpleExpression : public ExpressionBase
	{
	public:
		SimpleExpression(const std::string &name, const std::string &type, const std::string &value)
		{
			Type = type;
			Value = value;
		}
	};

	HLDPServer::HLDPServer(int tcpPort) : m_BreakInPending(true) { m_pSocket = new BasicIncomingSocket(tcpPort); }

	HLDPServer::~HLDPServer() { delete m_pSocket; }

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

		if (ReceiveRequest(reader) != HLDPPacketType::csHandshake)
		{
			cmSystemTools::Error("Failed to complete HLDP handshake.");
			return false;
		}

		return true;
	}

	std::unique_ptr<HLDPServer::RAIIScope> HLDPServer::OnExecutingInitialPass(cmCommand *pCommand, cmMakefile *pMakefile, const cmListFileFunction &function)
	{
		if (m_Detached)
			return nullptr;

		std::unique_ptr<RAIIScope> pScope(new RAIIScope(this, pCommand, pMakefile, function));

		UniqueScopeID parentScope = kRootScope;
		if (m_CallStack.size() >= 2)
		{
			parentScope = m_CallStack[m_CallStack.size() - 2]->GetUniqueID();
		}

		if (parentScope == m_EndOfStepScopeID)
			m_BreakInPending = true;

		if (!m_BreakInPending)
		{
			if (m_pSocket->HasIncomingData())
			{
				RequestReader reader;
				HLDPPacketType requestType = ReceiveRequest(reader);
				switch (requestType)
				{
				case HLDPPacketType::Invalid:
					return nullptr;
				case HLDPPacketType::csBreakIn:
					m_BreakInPending = true;
					break;
				default:
					SendErrorPacket("Unexpected packet received while the target is running");
					return nullptr;
				}
			}

			return std::move(pScope);
		}

		m_BreakInPending = false;
		m_EndOfStepScopeID = 0;
		ReplyBuilder builder;
		builder.AppendInt32((unsigned)TargetStopReason::InitialBreakIn);
		builder.AppendInt32(0);
		builder.AppendString("");

		auto backtraceEntryCount = builder.AppendDelayedInt32();

		for (int i = m_CallStack.size() - 1; i >= 0; i--)
		{
			auto *pEntry = m_CallStack[i];
			pCommand->GetMakefile()->GetBacktrace();
			builder.AppendInt32(i);

			std::string args;
			if (i == 0)
				builder.AppendString("");
			else
			{
				builder.AppendString(m_CallStack[i - 1]->Function.Name.Original);

				for (const auto &arg : m_CallStack[i - 1]->Function.Arguments)
				{
					if (args.length() > 0)
						args.append(", ");

					args.append(arg.Value);
				}
			}

			builder.AppendString(args);
			builder.AppendString(pEntry->SourceFile);
			builder.AppendInt32(pEntry->Function.Line);
			(*backtraceEntryCount)++;
		}

		if (!SendReply(HLDPPacketType::scTargetStopped, builder))
			return nullptr;

		for (;;)
		{
			builder.Reset();

			RequestReader reader;
			HLDPPacketType requestType = ReceiveRequest(reader);
			switch (requestType)
			{
			case HLDPPacketType::csBreakIn:
				// TODO: resend backtrace.
				continue; // The target is already stopped.
			case HLDPPacketType::csContinue:
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return std::move(pScope);
			case HLDPPacketType::csStepIn:
				m_BreakInPending = true;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return std::move(pScope);
			case HLDPPacketType::csStepOut:
				if (m_CallStack.size() >= 3)
					m_EndOfStepScopeID = m_CallStack[m_CallStack.size() - 3]->GetUniqueID();
				else if (m_CallStack.size() == 2)
					m_EndOfStepScopeID = kRootScope;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return std::move(pScope);
			case HLDPPacketType::csStepOver:
				m_EndOfStepScopeID = parentScope;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return std::move(pScope);
			case HLDPPacketType::csDetach:
				m_Detached = true;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return nullptr;
			case HLDPPacketType::csTerminate:
				cmSystemTools::Error("Configuration aborted via debugging interface.");
				cmSystemTools::SetFatalErrorOccured();
				return nullptr;
			case HLDPPacketType::csCreateExpression:
			{
				int frameID = 0;
				std::string expression;
				if (!reader.ReadInt32(&frameID) || !reader.ReadString(&expression))
				{
					SendErrorPacket("Invalid expression request");
					continue;
				}

				std::unique_ptr<ExpressionBase> pExpression;
				if (frameID < 0 || frameID >= m_CallStack.size())
					SendErrorPacket("Invalid frame ID");
				else
				{
					pExpression = CreateExpression(expression, *m_CallStack[frameID]);
					if (pExpression)
					{
						pExpression->AssignedID = m_NextExpressionID++;

						builder.AppendInt32(pExpression->AssignedID);
						builder.AppendString(pExpression->Value);
						builder.AppendString(pExpression->Type);
						builder.AppendInt32(0);

						m_ExpressionCache[pExpression->AssignedID] = std::move(pExpression);

						SendReply(HLDPPacketType::scExpressionCreated, builder);
					}
					else
					{
						SendErrorPacket("Failed to create expression");
					}
				}

				continue;
			}
			break;
			default:
				SendErrorPacket("Unexpected packet received while the target is stopped");
				break;
			}
		}

		m_ExpressionCache.clear();
		return std::move(pScope);
	}

	bool HLDPServer::SendReply(HLDPPacketType packetType, const ReplyBuilder &builder)
	{
		HLDPPacketHeader hdr = {(unsigned)packetType, builder.GetBuffer().size()};

		if (!m_pSocket->Write(&hdr, sizeof(hdr)))
		{
			cmSystemTools::Error("Failed to write debug protocol reply header.");
			cmSystemTools::SetFatalErrorOccured();
			return false;
		}

		if (!m_pSocket->Write(builder.GetBuffer().data(), hdr.PayloadSize))
		{
			cmSystemTools::Error("Failed to write debug protocol reply payload.");
			cmSystemTools::SetFatalErrorOccured();
			return false;
		}

		return true;
	}

	HLDPPacketType HLDPServer::ReceiveRequest(RequestReader &reader)
	{
		HLDPPacketHeader hdr;
		if (!m_pSocket->ReadAll(&hdr, sizeof(hdr)))
		{
			cmSystemTools::Error("Failed to receive debug protocol request header.");
			cmSystemTools::SetFatalErrorOccured();
			return HLDPPacketType::Invalid;
		}

		void *pBuffer = reader.Reset(hdr.PayloadSize);
		if (hdr.PayloadSize != 0)
		{
			if (!m_pSocket->ReadAll(pBuffer, hdr.PayloadSize))
			{
				cmSystemTools::Error("Failed to receive debug protocol request payload.");
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

	inline HLDPServer::RAIIScope::RAIIScope(HLDPServer *pServer, cmCommand *pCommand, cmMakefile *pMakefile, const cmListFileFunction &function)
		: m_pServer(pServer), Command(pCommand), Makefile(pMakefile), Function(function), m_UniqueID(pServer->m_NextScopeID++), Position(pMakefile->GetStateSnapshot().GetPositionForDebugging())
	{
		pServer->m_CallStack.push_back(this);
		SourceFile = Makefile->GetStateSnapshot().GetExecutionListFile();
	}

	HLDPServer::RAIIScope::~RAIIScope()
	{
		if (m_pServer->m_CallStack.back() != this)
		{
			cmSystemTools::Error("CMake scope imbalance detected");
			cmSystemTools::SetFatalErrorOccured();
		}

		if (m_UniqueID == m_pServer->m_EndOfStepScopeID)
			m_pServer->m_BreakInPending = true; // We are stepping out of a function scope where we were supposed to stop.

		m_pServer->m_CallStack.pop_back();
	}

	std::unique_ptr<HLDPServer::ExpressionBase> HLDPServer::CreateExpression(const std::string &text, const RAIIScope &scope)
	{
		const char *pValue = cmDefinitions::Get(text, scope.Position->Vars, scope.Position->Root);
		if (pValue)
		{
			return std::make_unique<SimpleExpression>(text, "(CMake Variable)", pValue);
		}

		return nullptr;
	}

} // namespace Sysprogs