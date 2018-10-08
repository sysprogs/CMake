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
		typedef std::pair<int, std::string> CanonicalFileLocation;

		struct BreakpointObject
		{
			UniqueBreakpointID AssignedID = 0;
			CanonicalFileLocation Location;
			bool IsEnabled = true;

			BreakpointObject(UniqueBreakpointID id, const CanonicalFileLocation &location)
			{
				AssignedID = id; 
				Location = location;
			}
		};

	public:
		BreakpointObject *TryGetBreakpointAtLocation(const std::string &file, int oneBasedLine);
		UniqueBreakpointID CreateBreakpoint(const std::string &file, int oneBasedLine);
		void DeleteBreakpoint(UniqueBreakpointID id);
		BreakpointObject *TryLookupBreakpointObject(UniqueBreakpointID id);

	private:
		UniqueBreakpointID m_NextID = 1;

		std::map<UniqueBreakpointID, std::unique_ptr<BreakpointObject>> m_BreakpointsByID;
		std::map<CanonicalFileLocation, std::set<UniqueBreakpointID>> m_BreakpointsByLocation;

		std::map<std::string, std::string> m_CanonicalPathMap;

		CanonicalFileLocation MakeCanonicalLocation(const std::string &file, int oneBasedLine);
	};
} // namespace Sysprogs