#pragma once
#include <string>
#include <set>
#include <map>

namespace Sysprogs
{
	class BasicBreakpointManager
	{
	public:
		typedef int UniqueBreakpointID;
		struct CanonicalFileLocation
		{
			std::string Path;
			int OneBasedLine;

			bool operator<(const CanonicalFileLocation &right) const
			{
				if (OneBasedLine != right.OneBasedLine)
					return OneBasedLine < right.OneBasedLine;
				return stricmp(Path.c_str(), right.Path.c_str()) < 0;
			}

			CanonicalFileLocation(const std::string &path = "", int line = 0) : Path(path), OneBasedLine(line) {}
		};

		struct CaseInsensitiveFunctionName
		{
			std::string Name;

			bool operator<(const CaseInsensitiveFunctionName &right) const { return stricmp(Name.c_str(), right.Name.c_str()) < 0; }

			CaseInsensitiveFunctionName(const std::string &name = "") : Name(name) {}
		};

		struct BreakpointObject
		{
			UniqueBreakpointID AssignedID = 0;

			CanonicalFileLocation Location;
			CaseInsensitiveFunctionName FunctionName;

			bool IsEnabled = true;

			BreakpointObject(UniqueBreakpointID id, const CanonicalFileLocation &location) : Location(location) { AssignedID = id; }
			BreakpointObject(UniqueBreakpointID id, const CaseInsensitiveFunctionName &name) : FunctionName(name) { AssignedID = id; }

		};

	public:
		BreakpointObject *TryGetBreakpointAtLocation(const std::string &file, int oneBasedLine);
		BreakpointObject *TryGetBreakpointForFunction(const std::string &function);

		UniqueBreakpointID CreateBreakpoint(const std::string &file, int oneBasedLine);
		UniqueBreakpointID CreateBreakpoint(const std::string &function);

		void DeleteBreakpoint(UniqueBreakpointID id);
		BreakpointObject *TryLookupBreakpointObject(UniqueBreakpointID id);

	private:
		UniqueBreakpointID m_NextID = 1;

		std::map<UniqueBreakpointID, std::unique_ptr<BreakpointObject>> m_BreakpointsByID;
		std::map<CanonicalFileLocation, std::set<UniqueBreakpointID>> m_BreakpointsByLocation;
		std::map<CaseInsensitiveFunctionName, std::set<UniqueBreakpointID>> m_BreakpointsByFunctionName;

		std::map<std::string, std::string> m_CanonicalPathMap;

		CanonicalFileLocation MakeCanonicalLocation(const std::string &file, int oneBasedLine);
	};
} // namespace Sysprogs