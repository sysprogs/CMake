#pragma once

namespace Sysprogs
{
	/*
		This file defines structures used in the Sysprogs High-Level Debug Protocol.
		The protocol is optimized for low latency and is designed to run on resource-limited embedded
		devices, hence all exchange is performed using binary packets.
	*/
	
	static const char HLDPBanner[] = "Sysprogs High-Level Debug Protocol";
	static const int HLDPVersion = 1;
	
	//Contains all HLDP packet types. Values prefixed with 'sc' are for Server-to-Client packets, 'cs' stands for Client-to-Server.
	enum class HLDPPacketType
	{
		Invalid,
		scError,			//Payload: error:string
		scHandshake,		//Payload: version:int32, subexpression delimiter:string
		csHandshake,		//No payload
		
		scTargetStopped,	//Payload: TargetStopReason:int32, IntArg:int32, StringArg:string, [array of BacktraceEntry]
		scTargetRunning,	//No payload
		
		//No payload for the flow control packets
		csContinue,
		csStepIn,
		csStepOut,
		csStepOver,
		csBreakIn,			//Requests the target to stop ASAP.
		
		//All expressions are automatically deleted once the target resumes or performs a step
		csCreateExpression,				//Payload: Expression:string
		scExpressionCreated,			//Payload: ID:int32, value:string, type:string, ChildCount:int32 (ChildCount = -1 indicates that the exact count will be computed later)
		csQueryExpressionChildren,		//Payload: ID:int32
		scExpressionChildrenQueried,	//Payload: array of [ID:int32, name:string, type:string, value:string]
	};
	
	enum class TargetStopReason : unsigned
	{
		InitialBreakIn,
	};
	
	struct HLDPPacketHeader
	{
		unsigned Type;
		unsigned PayloadSize;
	};
	
	/*
		Typical handshake sequence:
		1. Null-terminated HDLPBanner + scHandshake
		2. csHandshake
		3. scTargetStopped
			
		Common terms:
		* Subexpression delimiter - an operator (e.g. '.') that should never be a part of a valid expression for this target. The client will internally use it to make fully qualified paths
									of subexpressions and will automatically break it down into separate requests so the server doesn't need to do any advanced parsing.
									   
		
		Data types:
		* Array := [length:int32] [element #0] [element #1] ... [last element]
		* String := Array (see above) of UTF8 chars
		* BacktraceEntry := Unique Frame ID:int32, Function:string, Arguments:string, Source File:string, One-based line:int32
	*/
}