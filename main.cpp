#include "3rdparty/args.hxx"
#define CPPHTTPLIB_OPENSSL_SUPPORT // https 사용
#include "3rdparty/httplib.h"
#include "3rdparty/zip_file.hpp"
#include <memory>

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
		, help(parser, "help", "Display this help menu", { 'h', "help" })
		, versionUrl(parser, "versionUrl", "url for version file")
	{
		parser.ParseCLI(argc, argv);
	}

private:
	ArgumentParser parser; // 순서가 중요. 생성자와 상관없이 먼저 선언된 멤버의 생성자가 먼저 불린다.

public:
	HelpFlag help;
	Positional<string> versionUrl;

	const ArgumentParser& GetParser() { return parser; }
};

enum class AppResult : int
{
	OK = 0,

	PARAMETER_ERROR = 1,
};

size_t GetPathSepIndex(const string& url)
{
	int sep = 0;
	size_t i = 0;
	for (i = 0; i < url.size(); ++i)
	{
		if (url[i] == '/') ++sep;
		if (sep == 3) return i;
	}
	return i;
}

void StreamOut(const httplib::Result& result, ostream& out)
{
	out.write(result->body.c_str(), sizeof(char) * result->body.size());
}

string Replace(const string& src, const string& from, const string& to)
{
	string ret = src;
	string::size_type pos = 0;
	while ((pos = ret.find(from)) != string::npos)
	{
		ret.replace(pos, from.length(), to);
	}
	return ret;
}

string MakeCookieValue(const map<string, string>& cookies)
{
	stringstream ss;
	for (const auto& c : cookies)
	{
		ss << c.first << "=" << c.second << "; ";
	}
	return ss.str();
}

void AddCookie(const string& headerValue, map<string, string>& outCookies)
{
	auto equalPos = headerValue.find('=');
	if (equalPos == string::npos) return; // no key=value pair
	auto semicolonPos = headerValue.find(';');
	if (semicolonPos == string::npos) semicolonPos = headerValue.length();
	if (equalPos > semicolonPos) return; // invalid format

	auto key = headerValue.substr(0, equalPos);
	auto value = headerValue.substr(equalPos + 1, semicolonPos - (equalPos + 1));
	outCookies.insert({ key, value });
}

bool Request(const std::string& url, ostream& out, map<string, string> cookies = map<string, string>(), int recursiveCount = 0)
{
	++recursiveCount;
	if (recursiveCount > 5)
	{
		cerr << "too many rediection." << url << endl;
		return false;
	}

	auto sepPos = GetPathSepIndex(url);
	auto serverAddress = url.substr(0, sepPos);
	auto path = url.substr(sepPos);

	// preparing request header
	auto headers = httplib::Headers();
	if (cookies.empty() == false)
	{ // https://developer.mozilla.org/ko/docs/Web/HTTP/Cookies
		headers.insert({ "Cookie", MakeCookieValue(cookies) });
	}

	// requesting - GET
	httplib::Client versionClient(serverAddress.c_str());
	auto res = versionClient.Get(path.c_str(), headers);

	// storing cookies ; https://developer.mozilla.org/ko/docs/Web/HTTP/Cookies
	const auto& COOKIE_KEY = "Set-Cookie";
	auto cookieCount = res->get_header_value_count(COOKIE_KEY);
	for (size_t i = 0; i < cookieCount; ++i)
	{
		AddCookie(res->get_header_value(COOKIE_KEY, i), cookies);
	}

	// debug output(header)
	cerr << "STATUS(" << res->status << ") " << url << endl;
	cout << "  headers " << endl;
	for (auto i = res->headers.cbegin(); i != res->headers.cend(); ++i)
	{
		cout << "    " << i->first << " = " << i->second << endl;
	}
	//cout << "body = " << res->body << endl << endl;

	// result handling
	if (res->status == 200) // OK
	{
		// google-drive-specific ; 대용량 파일의 경우 virus 검사 할 수 없다며 별도의 링크를 요구
		if (url.find("://drive.google.com/") < 6)
		{
			auto hrefPos = res->body.find("href=\"/uc?export=download&amp;confirm=");
			if (hrefPos != string::npos)
			{
				auto hrefEndPos = res->body.find('"', hrefPos + 6);
				auto link = res->body.substr(hrefPos + 6, hrefEndPos - (hrefPos + 6));
				link = Replace(link, "&amp;", "&");
				auto schemePos = link.find_first_not_of("://");
				auto fullLinkUrl = (schemePos == 4 /*http*/ || schemePos == 5 /*https*/)
					? link
					: serverAddress + link;

				return Request(fullLinkUrl, out, cookies, recursiveCount);
			}
		}

		StreamOut(res, out);
		return true;
	}

	// http status error handling

	if (res->status == 302) // redirection
	{
		auto redirectTo = res->get_header_value("Location");
		if (redirectTo.empty())
		{
			cerr << "Location not found to redirect. " << url << endl;
			return false;
		}
		return Request(redirectTo, out, cookies, recursiveCount);
	}

	return false;
}

bool IsSameFile(const string& fileA, const string& fileB)
{ // https://stackoverflow.com/questions/6163611/compare-two-files/37575457
	ifstream f1(fileA, ifstream::binary | ifstream::ate);
	ifstream f2(fileB, ifstream::binary | ifstream::ate);
	if (f1.fail() || f2.fail()) return false; // file problem
	if (f1.tellg() != f2.tellg()) return false; // size mismatch

	// seek back to beginning and use std::equal to compare contents
	f1.seekg(0, std::ifstream::beg);
	f2.seekg(0, std::ifstream::beg);
	return std::equal(std::istreambuf_iterator<char>(f1.rdbuf()),
		std::istreambuf_iterator<char>(),
		std::istreambuf_iterator<char>(f2.rdbuf()));
}

string ReadFirstLine(const string& filePath)
{
	ifstream f(filePath);
	string line;
	getline(f, line);
	return line;
}

bool Exists(const string& filePath)
{
	ifstream f(filePath);
	return f.good();
}

int main(int argc, const char** argv)
{
	Arguments args(argc, argv);

	if (argc < 2)
	{
		cout << args.GetParser();
		return static_cast<int>(AppResult::PARAMETER_ERROR);
	}

	//Request(args.versionUrl.Get(), cout);
	//Request("https://drive.google.com/uc?export=download&id=1FiOh_Md6trjNC1qigOZhLmQDc7Wv1HxQ", cout);
	ofstream test("test.zip", ofstream::binary);
	Request("https://drive.google.com/uc?export=download&id=1FiOh_Md6trjNC1qigOZhLmQDc7Wv1HxQ", test);
	//Request("https://drive.google.com/u/0/uc?export=download&confirm=sqnw&id=1FiOh_Md6trjNC1qigOZhLmQDc7Wv1HxQ", test);
	return static_cast<int>(AppResult::OK);
}