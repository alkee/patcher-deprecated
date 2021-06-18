#include "3rdparty/args.hxx"
#include "3rdparty/httplib.h"
#include "3rdparty/zip_file.hpp"

using namespace args;
using namespace std;

string BuildHelpEpliog(const char* appName)
{
	stringstream epilog;

	epilog
		<< endl
		<< "  Examples:" << endl
		<< "    " << appName << endl
		<< "        ; display this help document" << endl
		<< "    " << appName << " https://drive.google.com/uc?export=download&id=1Yv0YNCYH539R0atZ8b0kxlCRSXampzxK" << endl
		<< "        ; just patch and exit" << endl
		;

	return epilog.str();
}

class Arguments
{
public:
	Arguments(int argc, const char** argv)
		: parser("minimal patcher & runner by alkee", BuildHelpEpliog("patcher.exe"))
		, help(parser, "help", "Display this help menu", { 'h', "help" }), Help(help)
		, versionUrl(parser, "versionUrl", "url for version file"), VersionUrl(versionUrl)
	{
	}

	const HelpFlag& Help;
	const Positional<string>& VersionUrl;

	const ArgumentParser& GetParser() { return parser; }

private:
	ArgumentParser parser;
	HelpFlag help;
	Positional<string> versionUrl;
};

enum class AppResult : int
{
	OK = 0,

	PARAMETER_ERROR = 1,
};

int main(int argc, const char** argv)
{
	Arguments args(argc, argv);

	if (argc < 2)
	{
		cout << args.GetParser();
		return static_cast<int>(AppResult::PARAMETER_ERROR);
	}

	return static_cast<int>(AppResult::OK);
}
