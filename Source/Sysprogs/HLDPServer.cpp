#include "cmCommand.h"
#include "cmMakefile.h"
#include "BasicIncomingSocket.h"
#include "HLDP.h"
#include "HLDPServer.h"
#include "cmSystemTools.h"
#include "cmStatePrivate.h"
#include "cmVariableWatch.h"
#include "cmake.h"
#include "cmMessageType.h"
#include "cmCacheManager.h"
#include "cmsys/String.h"

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

#pragma region Expressions
	class HLDPServer::SimpleExpression : public ExpressionBase
	{
	public:
		SimpleExpression(const std::string &name, const std::string &type, const std::string &value)
		{
			Name = name;
			Type = type;
			Value = value;
		}
	};

	class HLDPServer::VariableExpression : public ExpressionBase
	{
	private:
		const RAIIScope &m_Scope;

	public:
		VariableExpression(const RAIIScope &scope, const std::string &name, const char *pValue) : m_Scope(scope)
		{
			Name = name;
			Type = "(CMake Expression)";
			Value = pValue;
		}

		virtual bool UpdateValue(const std::string &value, std::string &error) override
		{
			auto &entry = cmDefinitions::GetInternal(Name, m_Scope.Position->Vars, m_Scope.Position->Root, false);
			if (entry.Value)
			{
				const_cast<cm::String &>(entry.Value) = value;
				return true;
			}
			else
			{
				error = "Unable to find variable: " + Name;
				return false;
			}
		}
	};

	class HLDPServer::CacheEntryExpression : public ExpressionBase
	{
	public:
		CacheEntryExpression(const std::string &name, const char *pValue)
		{
			Name = name;
			Type = "(CMake Expression)";
			Value = pValue;
		}

		virtual bool UpdateValue(const std::string &value, std::string &error) override
		{ 
			return false;
		}
	};

	class HLDPServer::EnvironmentVariableExpression : public ExpressionBase
	{
	private:
		std::string m_VarName;

	public:
		EnvironmentVariableExpression(const std::string &name, const std::string &value, bool fromEnvList = false) : m_VarName(name)
		{
			if (!fromEnvList)
				Name = "ENV{" + name + "}";
			else
				Name = "[" + name + "]";

			Type = "(Environment Variable)";
			Value = value;
		}

		virtual bool UpdateValue(const std::string &value, std::string &error) override
		{
			cmSystemTools::PutEnv(m_VarName + "=" + value);
			return true;
		}
	};

	class HLDPServer::EnvironmentMetaExpression : public ExpressionBase
	{
	public:
		EnvironmentMetaExpression()
		{
			Name = "$ENV";
			Type = "(CMake Environment)";
			Value = "<...>";
			ChildCountOrMinusOneIfNotYetComputed = -1;
		}

		virtual std::vector<std::unique_ptr<ExpressionBase>> CreateChildren()
		{
			std::vector<std::unique_ptr<ExpressionBase>> result;
			for (const auto &kv : cmSystemTools::GetEnvironmentVariables())
			{
				int idx = kv.find('=');
				if (idx == std::string::npos)
					continue;
				result.push_back(std::make_unique<EnvironmentVariableExpression>(kv.substr(0, idx), kv.substr(idx + 1), true));
			}
			return std::move(result);
		}
	};

	class HLDPServer::TargetExpression : public ExpressionBase
	{
	private:
		cmTarget *m_pTarget;

	public:
		TargetExpression(cmTarget *pTarget) : m_pTarget(pTarget)
		{
			Type = "(CMake target)";
			Name = pTarget->GetName();
			Value = "target";
			ChildCountOrMinusOneIfNotYetComputed = -1;
		}

		virtual std::vector<std::unique_ptr<ExpressionBase>> CreateChildren() override
		{
			std::vector<std::unique_ptr<ExpressionBase>> result;
			const cmPropertyMap &properties = m_pTarget->GetProperties();
			std::string empty;
			for (const std::string &key : properties.GetKeys())
			{
				cmProp value = properties.GetPropertyValue(key);
				result.push_back(std::make_unique<SimpleExpression>(key, "(property entry)", value ? *value : empty));
			}
			return std::move(result);
		}
	};
#pragma endregion

	enum class CMakeDomainSpecificBreakpointType
	{
		VariableAccessed,
		VariableUpdated,
		MessageSent,
		TargetCreated,
	};

	class HLDPServer::DomainSpecificBreakpoint : public BasicBreakpointManager::DomainSpecificBreakpointExtension
	{
	public:
		CMakeDomainSpecificBreakpointType Type;
		std::string StringArg;

		DomainSpecificBreakpoint(const std::string &stringArg, int intArg) : StringArg(stringArg), Type((CMakeDomainSpecificBreakpointType)intArg) {}
	};

	HLDPServer::HLDPServer(int tcpPort) : m_BreakInPending(true) { m_pSocket = new BasicIncomingSocket(tcpPort); }

	HLDPServer::~HLDPServer()
	{
		ReplyBuilder builder;
		builder.AppendInt32(0);
		SendReply(HLDPPacketType::scTargetExited, builder);
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

		if (ReceiveRequest(reader) != HLDPPacketType::csHandshake)
		{
			cmSystemTools::Error("Failed to complete HLDP handshake.");
			return false;
		}

		return true;
	}

	std::unique_ptr<HLDPServer::RAIIScope> HLDPServer::OnExecutingInitialPass(cmMakefile *pMakefile, const cmListFileFunction &function, bool &skipThisInstruction)
	{
		skipThisInstruction = false;
		if (m_Detached)
			return nullptr;

		std::unique_ptr<RAIIScope> pScope(new RAIIScope(this, pMakefile, function));
		struct
		{
			TargetStopReason reason = TargetStopReason::UnspecifiedEvent;
			unsigned intParam = 0;
			std::string stringParam;
		} stopRecord;

		{
			auto *pBreakpoint = m_BreakpointManager.TryGetBreakpointAtLocation(pScope->SourceFile, pScope->Function.Line());
			if (!pBreakpoint)
				pBreakpoint = m_BreakpointManager.TryGetBreakpointForFunction(function.OriginalName());

			if (pBreakpoint && pBreakpoint->IsEnabled)
			{
				m_BreakInPending = true;
				stopRecord.intParam = pBreakpoint->AssignedID;
				stopRecord.reason = TargetStopReason::Breakpoint;
			}
		}

		UniqueScopeID parentScope = kRootScope;
		if (m_CallStack.size() >= 2)
		{
			parentScope = m_CallStack[m_CallStack.size() - 2]->GetUniqueID();
		}

		if (parentScope == m_EndOfStepScopeID)
		{
			m_BreakInPending = true;
			if (stopRecord.reason == TargetStopReason::UnspecifiedEvent)
				stopRecord.reason = TargetStopReason::StepComplete;
		}

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
					stopRecord.reason = TargetStopReason::BreakInRequested;
					m_BreakInPending = true;
					break;
				default:
					if (requestType > HLDPPacketType::BeforeFirstBreakpointRelatedCommand && requestType < HLDPPacketType::AfterLastBreakpointRelatedCommand)
					{
						HandleBreakpointRelatedCommand(requestType, reader);
					}
					else
						SendErrorPacket("Unexpected packet received while the target is running");
					return nullptr;
				}
			}

			return std::move(pScope);
		}

		if (m_NextOneBasedLineToExecute && stopRecord.reason == TargetStopReason::UnspecifiedEvent)
			stopRecord.reason = TargetStopReason::SetNextStatement;

		if (!m_EventsReported && stopRecord.reason == TargetStopReason::UnspecifiedEvent)
			stopRecord.reason = TargetStopReason::InitialBreakIn;

		m_EventsReported = true;
		ReportStopAndServeDebugRequests(stopRecord.reason, stopRecord.intParam, stopRecord.stringParam, &skipThisInstruction);
		return std::move(pScope);
	}

	void HLDPServer::AdjustNextExecutedFunction(const std::vector<cmListFileFunction> &functions, size_t &i)
	{
		if (m_NextOneBasedLineToExecute)
		{
			for (int j = 0; j < functions.size(); j++)
			{
				if (functions[j].Line() == m_NextOneBasedLineToExecute)
				{
					i = j;
					return;
				}
			}
		}
	}

	void HLDPServer::OnMessageProduced(MessageType type, const std::string &message)
	{
		ReplyBuilder builder;
		builder.AppendInt32(0);
		builder.AppendString(message);
		SendReply(HLDPPacketType::scDebugMessage, builder);

		switch (type)
		{
		case MessageType::FATAL_ERROR:
		case MessageType::INTERNAL_ERROR:
		case MessageType::AUTHOR_ERROR:
		case MessageType::DEPRECATION_ERROR:
			ReportStopAndServeDebugRequests(TargetStopReason::Exception, 0, message, nullptr);
			return;
		}

		auto id = m_BreakpointManager.TryLocateEnabledDomainSpecificBreakpoint([&](BasicBreakpointManager::DomainSpecificBreakpointExtension *pBp) {
			switch (static_cast<DomainSpecificBreakpoint *>(pBp)->Type)
			{
			case CMakeDomainSpecificBreakpointType::MessageSent:
				if (message.find(static_cast<DomainSpecificBreakpoint *>(pBp)->StringArg) != std::string::npos)
					return true;
				break;
			}

			return false;
		});

		if (id)
			ReportStopAndServeDebugRequests(TargetStopReason::Breakpoint, id, "", nullptr);
	}

	void HLDPServer::OnVariableAccessed(const std::string &variable, int access_type, const char *newValue, const cmMakefile *mf)
	{
		if (m_WatchedVariables.find(variable) == m_WatchedVariables.end())
			return;

		bool isRead = access_type == cmVariableWatch::VARIABLE_READ_ACCESS || access_type == cmVariableWatch::UNKNOWN_VARIABLE_READ_ACCESS;

		auto id = m_BreakpointManager.TryLocateEnabledDomainSpecificBreakpoint([&](BasicBreakpointManager::DomainSpecificBreakpointExtension *pBp) {
			switch (static_cast<DomainSpecificBreakpoint *>(pBp)->Type)
			{
			case CMakeDomainSpecificBreakpointType::VariableAccessed:
			case CMakeDomainSpecificBreakpointType::VariableUpdated:
				if (isRead == (static_cast<DomainSpecificBreakpoint *>(pBp)->Type == CMakeDomainSpecificBreakpointType::VariableAccessed) &&
					variable == static_cast<DomainSpecificBreakpoint *>(pBp)->StringArg)
					return true;
				break;
			}

			return false;
		});

		if (id)
			ReportStopAndServeDebugRequests(TargetStopReason::Breakpoint, id, "", nullptr);
	}

	void HLDPServer::OnTargetCreated(cmStateEnums::TargetType type, const std::string &targetName)
	{
		auto id = m_BreakpointManager.TryLocateEnabledDomainSpecificBreakpoint([&](BasicBreakpointManager::DomainSpecificBreakpointExtension *pBp) {
			switch (static_cast<DomainSpecificBreakpoint *>(pBp)->Type)
			{
			case CMakeDomainSpecificBreakpointType::TargetCreated:
				if (static_cast<DomainSpecificBreakpoint *>(pBp)->StringArg.empty() || targetName == static_cast<DomainSpecificBreakpoint *>(pBp)->StringArg)
					return true;
				break;
			}

			return false;
		});

		if (id)
			ReportStopAndServeDebugRequests(TargetStopReason::Breakpoint, id, "", nullptr);
	}

	void HLDPServer::HandleBreakpointRelatedCommand(HLDPPacketType type, RequestReader &reader)
	{
		std::string file, stringArg;
		int line, intArg1, intArg2;
		ReplyBuilder builder;
		BasicBreakpointManager::UniqueBreakpointID id;

		struct
		{
			BreakpointField field;
			int IntArg1, IntArg2;
			std::string StringArg;
		} breakpointUpdateRequest;

		switch (type)
		{
		case HLDPPacketType::csCreateBreakpoint:
			if (!reader.ReadString(&file) || !reader.ReadInt32(&line))
				SendErrorPacket("Invalid breakpoint request");
			else
			{
				id = m_BreakpointManager.CreateBreakpoint(file, line);
				if (!id)
					SendErrorPacket("Invalid or non-existent file: " + file);
				else
				{
					builder.AppendInt32(id);
					SendReply(HLDPPacketType::scBreakpointCreated, builder);
				}
			}
			break;
		case HLDPPacketType::csCreateFunctionBreakpoint:
			if (!reader.ReadString(&stringArg))
				SendErrorPacket("Invalid breakpoint request");
			else
			{
				id = m_BreakpointManager.CreateBreakpoint(stringArg);
				if (!id)
					SendErrorPacket("Failed to create a function breakpoint for " + stringArg);
				else
				{
					builder.AppendInt32(id);
					SendReply(HLDPPacketType::scBreakpointCreated, builder);
				}
			}
			break;
		case HLDPPacketType::csCreateDomainSpecificBreakpoint:
			if (!reader.ReadInt32(&intArg1) || !reader.ReadString(&stringArg) || !reader.ReadInt32(&intArg2))
				SendErrorPacket("Invalid breakpoint request");
			else
			{
				id = m_BreakpointManager.CreateDomainSpecificBreakpoint(std::make_unique<DomainSpecificBreakpoint>(stringArg, intArg1));
				if (!id)
					SendErrorPacket("Failed to create a CMake breakpoint");
				else
				{
					if (intArg1 == (int)CMakeDomainSpecificBreakpointType::VariableAccessed || intArg1 == (int)CMakeDomainSpecificBreakpointType::VariableUpdated)
						m_WatchedVariables.insert(stringArg);

					builder.AppendInt32(id);
					SendReply(HLDPPacketType::scBreakpointCreated, builder);
				}
			}
			break;
		case HLDPPacketType::csDeleteBreakpoint:
			if (!reader.ReadInt32(&id))
				SendErrorPacket("Invalid breakpoint request");
			else
			{
				m_BreakpointManager.DeleteBreakpoint(id);
				SendReply(HLDPPacketType::scBreakpointUpdated, builder);
			}
			break;
		case HLDPPacketType::csUpdateBreakpoint:
			if (!reader.ReadInt32(&id) || !reader.ReadInt32((int *)&breakpointUpdateRequest.field) || !reader.ReadInt32(&breakpointUpdateRequest.IntArg1) ||
				!reader.ReadInt32(&breakpointUpdateRequest.IntArg2) || !reader.ReadString(&breakpointUpdateRequest.StringArg))
				SendErrorPacket("Invalid breakpoint request");
			else
			{
				auto *pBreakpoint = m_BreakpointManager.TryLookupBreakpointObject(id);
				if (!pBreakpoint)
					SendErrorPacket("Could not find a breakpoint with the specified ID");
				else
				{
					switch (breakpointUpdateRequest.field)
					{
					case BreakpointField::IsEnabled:
						pBreakpoint->IsEnabled = breakpointUpdateRequest.IntArg1 != 0;
						SendReply(HLDPPacketType::scBreakpointUpdated, builder);
						break;
					default:
						SendErrorPacket("Invalid breakpoint field");
					}
				}
			}
		}
	}

	bool HLDPServer::SendReply(HLDPPacketType packetType, const ReplyBuilder &builder)
	{
		HLDPPacketHeader hdr = {(unsigned)packetType, (unsigned)builder.GetBuffer().size()};

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

	void HLDPServer::ReportStopAndServeDebugRequests(TargetStopReason stopReason, unsigned intParam, const std::string &stringParam, bool *pSkipThisInstruction)
	{
		m_BreakInPending = false;
		m_EndOfStepScopeID = 0;
		m_NextOneBasedLineToExecute = 0;

		ReplyBuilder builder;
		builder.AppendInt32((unsigned)stopReason);
		builder.AppendInt32(intParam);
		builder.AppendString(stringParam);

		auto backtraceEntryCount = builder.AppendDelayedInt32();

		for (int i = m_CallStack.size() - 1; i >= 0; i--)
		{
			auto *pEntry = m_CallStack[i];
			builder.AppendInt32(i);

			std::string args;
			if (i == 0)
				builder.AppendString("");
			else
			{
				builder.AppendString(m_CallStack[i - 1]->Function.OriginalName());

				for (const auto &arg : m_CallStack[i - 1]->Function.Arguments())
				{
					if (args.length() > 0)
						args.append(", ");

					args.append(arg.Value);
				}
			}

			builder.AppendString(args);
			builder.AppendString(pEntry->SourceFile);
			builder.AppendInt32(pEntry->Function.Line());
			(*backtraceEntryCount)++;
		}

		if (!SendReply(HLDPPacketType::scTargetStopped, builder))
			return;

		int ID = 0;
		std::string expression;

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
				m_EndOfStepScopeID = kNoScope;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return;
			case HLDPPacketType::csStepIn:
				m_BreakInPending = true;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return;
			case HLDPPacketType::csStepOut:
				if (m_CallStack.size() >= 3)
					m_EndOfStepScopeID = m_CallStack[m_CallStack.size() - 3]->GetUniqueID();
				else if (m_CallStack.size() == 2)
					m_EndOfStepScopeID = kRootScope;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return;
			case HLDPPacketType::csStepOver:
				if (m_CallStack.size() >= 2)
					m_EndOfStepScopeID = m_CallStack[m_CallStack.size() - 2]->GetUniqueID();
				else
					m_EndOfStepScopeID = kRootScope;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return;
			case HLDPPacketType::csSetNextStatement:
				if (!pSkipThisInstruction)
				{
					SendErrorPacket("Cannot set next statement in this context");
					continue;
				}
				else if (!reader.ReadString(&expression) || !reader.ReadInt32(&ID))
				{
					SendErrorPacket("Invalid set-next-statement request");
					continue;
				}
				else if (m_CallStack.size() == 0)
				{
					SendErrorPacket("Unknown CMake call stack");
					continue;
				}
				else
				{
					std::string canonicalRequestedPath = cmsys::SystemTools::GetRealPath(expression);
					std::string canonicalCurrentPath = cmsys::SystemTools::GetRealPath(m_CallStack[m_CallStack.size() - 1]->SourceFile);
					if (cmsysString_strcasecmp(canonicalCurrentPath.c_str(), canonicalRequestedPath.c_str()) != 0)
						SendErrorPacket("Cannot step to a different source file");
					else
					{
						m_NextOneBasedLineToExecute = ID;
						m_BreakInPending = true;
						*pSkipThisInstruction = true;
						SendReply(HLDPPacketType::scTargetRunning, builder);
						return;
					}
				}
			case HLDPPacketType::csDetach:
				m_Detached = true;
				SendReply(HLDPPacketType::scTargetRunning, builder);
				return;
			case HLDPPacketType::csTerminate:
				cmSystemTools::Error("Configuration aborted via debugging interface.");
				cmSystemTools::SetFatalErrorOccured();
				return;
			case HLDPPacketType::csCreateExpression:
			{
				if (!reader.ReadInt32(&ID) || !reader.ReadString(&expression))
				{
					SendErrorPacket("Invalid expression request");
					continue;
				}

				std::unique_ptr<ExpressionBase> pExpression;
				if (ID < 0 || ID >= m_CallStack.size())
					SendErrorPacket("Invalid frame ID");
				else
				{
					pExpression = CreateExpression(expression, *m_CallStack[ID]);
					if (pExpression)
					{
						pExpression->AssignedID = m_NextExpressionID++;

						builder.AppendInt32(pExpression->AssignedID);
						builder.AppendString(pExpression->Name);
						builder.AppendString(pExpression->Type);
						builder.AppendString(pExpression->Value);
						builder.AppendInt32(0);
						builder.AppendInt32(pExpression->ChildCountOrMinusOneIfNotYetComputed);

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
			case HLDPPacketType::csQueryExpressionChildren:
				if (!reader.ReadInt32(&ID))
				{
					SendErrorPacket("Invalid expression request");
					continue;
				}
				else
				{
					auto it = m_ExpressionCache.find(ID);
					if (it == m_ExpressionCache.end())
					{
						SendErrorPacket("Invalid expression ID");
						continue;
					}
					else
					{
						if (!it->second->ChildrenRegistered)
						{
							it->second->ChildrenRegistered = true;
							for (auto &pChild : it->second->CreateChildren())
							{
								pChild->AssignedID = m_NextExpressionID++;
								it->second->RegisteredChildren.push_back(pChild->AssignedID);
								m_ExpressionCache[pChild->AssignedID] = std::move(pChild);
							}
						}

						auto childCount = builder.AppendDelayedInt32();
						for (auto &id : it->second->RegisteredChildren)
						{
							auto it = m_ExpressionCache.find(id);
							if (it == m_ExpressionCache.end())
								continue;

							builder.AppendInt32(id);
							builder.AppendString(it->second->Name);
							builder.AppendString(it->second->Type);
							builder.AppendString(it->second->Value);
							builder.AppendInt32(0);
							builder.AppendInt32(it->second->ChildCountOrMinusOneIfNotYetComputed);

							(*childCount)++;
						}

						SendReply(HLDPPacketType::scExpressionChildrenQueried, builder);
					}
				}
				break;
			case HLDPPacketType::csSetExpressionValue:
				if (!reader.ReadInt32(&ID) || !reader.ReadString(&expression))
				{
					SendErrorPacket("Invalid expression request");
					continue;
				}
				else
				{
					auto it = m_ExpressionCache.find(ID);
					if (it == m_ExpressionCache.end())
					{
						SendErrorPacket("Invalid expression ID");
						continue;
					}
					std::string error;
					if (it->second->UpdateValue(expression, error))
						SendReply(HLDPPacketType::scExpressionUpdated, builder);
					else
						SendErrorPacket(error);
				}
				break;
			default:
				if (requestType > HLDPPacketType::BeforeFirstBreakpointRelatedCommand && requestType < HLDPPacketType::AfterLastBreakpointRelatedCommand)
				{
					HandleBreakpointRelatedCommand(requestType, reader);
				}
				else
					SendErrorPacket("Unexpected packet received while the target is stopped");
				break;
			}
		}

		m_ExpressionCache.clear();
	}

	inline HLDPServer::RAIIScope::RAIIScope(HLDPServer *pServer, cmMakefile *pMakefile, const cmListFileFunction &function)
		: m_pServer(pServer), Makefile(pMakefile), Function(function), m_UniqueID(pServer->m_NextScopeID++), Position(pMakefile->GetStateSnapshot().GetPositionForDebugging())
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
		if (text == "ENV" || text == "$ENV")
			return std::make_unique<EnvironmentMetaExpression>();
		if (cmHasLiteralPrefix(text, "ENV{") && text.size() > 5)
		{
			std::string varName = text.substr(4, text.size() - 5);
			std::string value;
			if (cmSystemTools::GetEnv(varName, value))
			{
				return std::make_unique<EnvironmentVariableExpression>(varName, value);
			}
			else
				return nullptr;
		}

		const std::string *pValue = cmDefinitions::Get(text, scope.Position->Vars, scope.Position->Root);
		if (pValue)
			return std::make_unique<VariableExpression>(scope, text, pValue->c_str());

		cmTarget *pTarget = scope.Makefile->FindTargetToUse(text, false);
		if (pTarget)
			return std::make_unique<TargetExpression>(pTarget);

		cmProp pCacheValue = scope.Makefile->GetState()->GetCacheEntryValue(text);
		if (pCacheValue)
		{
			return std::make_unique<CacheEntryExpression>(text, pCacheValue->c_str());
		}

		return nullptr;
	}

} // namespace Sysprogs
