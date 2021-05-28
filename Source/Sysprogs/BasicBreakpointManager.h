#pragma once
#include <string>
#include <set>
#include <map>
#include <memory>
#include "cmsys/String.h"

namespace Sysprogs
{
	class BasicBreakpointManager
	{
	public:
		typedef int UniqueBreakpointID;
		enum
		{
			InvalidBreakpointID = 0
		};

		struct CanonicalFileLocation
		{
			std::string Path;
			int OneBasedLine;

			bool operator<(const CanonicalFileLocation &right) const
			{
				if (OneBasedLine != right.OneBasedLine)
					return OneBasedLine < right.OneBasedLine;
				return cmsysString_strcasecmp(Path.c_str(), right.Path.c_str()) < 0;
			}

			CanonicalFileLocation(const std::string &path = "", int line = 0) : Path(path), OneBasedLine(line) {}
		};

		class DomainSpecificBreakpointExtension
		{
		public:
			virtual ~DomainSpecificBreakpointExtension() {}
		};

		struct CaseInsensitiveObjectName
		{
			std::string Name;

			bool operator<(const CaseInsensitiveObjectName &right) const { return cmsysString_strcasecmp(Name.c_str(), right.Name.c_str()) < 0; }

			CaseInsensitiveObjectName(const std::string &name = "") : Name(name) {}
		};

		struct BreakpointObject
		{
			UniqueBreakpointID AssignedID = InvalidBreakpointID;

			CanonicalFileLocation Location;
			CaseInsensitiveObjectName FunctionName;
			std::unique_ptr<DomainSpecificBreakpointExtension> Extension;

			bool IsEnabled = true;

			BreakpointObject(UniqueBreakpointID id, const CanonicalFileLocation &location) : Location(location) { AssignedID = id; }
			BreakpointObject(UniqueBreakpointID id, const CaseInsensitiveObjectName &name) : FunctionName(name) { AssignedID = id; }
			BreakpointObject(UniqueBreakpointID id, std::unique_ptr<DomainSpecificBreakpointExtension> &&extension) : Extension(std::move(extension)) { AssignedID = id; }
		};

	public:
		BreakpointObject *TryGetBreakpointAtLocation(const std::string &file, int oneBasedLine);
		BreakpointObject *TryGetBreakpointForFunction(const std::string &function);

		UniqueBreakpointID CreateBreakpoint(const std::string &file, int oneBasedLine);
		UniqueBreakpointID CreateBreakpoint(const std::string &function);
		UniqueBreakpointID CreateDomainSpecificBreakpoint(std::unique_ptr<DomainSpecificBreakpointExtension> &&extension);

		void DeleteBreakpoint(UniqueBreakpointID id);
		BreakpointObject *TryLookupBreakpointObject(UniqueBreakpointID id);

		template <class _Predicate> UniqueBreakpointID TryLocateEnabledDomainSpecificBreakpoint(_Predicate predicate)
		{
			for (const auto &kv : m_BreakpointsByID)
			{
				if (kv.second->Extension && kv.second->IsEnabled)
				{
					if (predicate(kv.second->Extension.get()))
						return kv.second->AssignedID;
				}
			}

			return InvalidBreakpointID;
		}

	private:
		UniqueBreakpointID m_NextID = 1;

		std::map<UniqueBreakpointID, std::unique_ptr<BreakpointObject>> m_BreakpointsByID;
		std::map<CanonicalFileLocation, std::set<UniqueBreakpointID>> m_BreakpointsByLocation;
		std::map<CaseInsensitiveObjectName, std::set<UniqueBreakpointID>> m_BreakpointsByFunctionName;

		std::map<std::string, std::string> m_CanonicalPathMap;

		CanonicalFileLocation MakeCanonicalLocation(const std::string &file, int oneBasedLine);
	};
} // namespace Sysprogs