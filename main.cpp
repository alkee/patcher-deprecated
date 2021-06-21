#include <filesystem> // c++17 필요

#include "3rdparty/args.hxx"
#define CPPHTTPLIB_OPENSSL_SUPPORT // https 사용
#include "3rdparty/httplib.h"
#include "3rdparty/zip_file.hpp"
#include "3rdparty/json_struct.h"

using namespace args;
using namespace std;

string BuildHelpEpliog(const char* appName)
{
    stringstream epilog;

    epilog
        << endl
        << "versionUrl must have version string in first line and zip filed to download in second line" << endl
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

template<class T>
bool LoadFrom(T& dest, const string& json)
{
    JS::ParseContext context(json);
    if (context.parseTo(dest) == JS::Error::NoError) return true;

    cerr << "verison file json error(" << static_cast<int>(context.error) << ")" << endl;
    cerr << "    " << context.makeErrorString() << endl; // detail error display
    return false;
}

struct AppConfig
{
    string VersionUrl;
    JS_OBJ(VersionUrl);

    bool Load(const string& configJson)
    {
        return LoadFrom(*this, configJson);
    }
};

enum class AppResult : int
{
    OK = 0,

    PARAMETER_ERROR = 1,
    FILESYSTEM_ERROR = 2,
    REQUEST_ERROR = 3,

    VERSION_JOSN_ERROR = 11,
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
    if (res.error() != httplib::Error::Success)
    {
        cerr << "http client error(" << res.error() << ") : " << url << endl;
        return false;
    }

    // storing cookies ; https://developer.mozilla.org/ko/docs/Web/HTTP/Cookies
    const auto& COOKIE_KEY = "Set-Cookie";
    auto cookieCount = res->get_header_value_count(COOKIE_KEY);
    for (size_t i = 0; i < cookieCount; ++i)
    {
        AddCookie(res->get_header_value(COOKIE_KEY, i), cookies);
    }

    // debug output(header)
    //cout << "STATUS(" << res->status << ") " << url << endl;
    //cout << "  headers " << endl;
    //for (auto i = res->headers.cbegin(); i != res->headers.cend(); ++i)
    //{
    //    cout << "    " << i->first << " = " << i->second << endl;
    //}
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

        out.write(res->body.c_str(), sizeof(char) * res->body.size());
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

bool Download(const string& url, const string& filePath)
{
    ofstream file(filePath, ofstream::binary);
    if (file.fail())
    {
        cerr << "could not write a file : " << filePath << endl;
        return false;
    }
    return Request(url, file);
}

std::string ReadTextFrom(const string& filePath)
{
    if (filesystem::exists(filePath) == false)
    {
        cerr << "file not found to read. " << filePath << endl;
        return "";
    }
    ifstream file(filePath, ifstream::binary);
    if (file.is_open() == false)
    {
        cerr << "file could not be opened. " << filePath << endl;
        return "";
    }
    return string(istreambuf_iterator<char>(file), istreambuf_iterator<char>());
}

bool ExtractZip(const filesystem::path& src, filesystem::path dest, bool slicent)
{
    if (!slicent) cout << "reading " << src.u8string() << endl;

    if (dest.empty()) dest = "."; // current directory

    try
    {
        miniz_cpp::zip_file zip(src.u8string()); // 잘못된 파일인 경우 std::runtime_error

        for (const auto& f : zip.infolist())
        {
            if (!slicent) cout << "extracting " << f.filename << " ... ";
            if (f.filename.back() == '/') // directory
            {
                const auto& target = dest / f.filename;
                if (filesystem::exists(target))
                {
                    if (!slicent) cout << "exists" << endl;
                    continue;
                }
                filesystem::create_directories(target); // 모든 상위 directory 를 생성하지 못하는 경우,
                                                     // 실제 directory 를 올바르게 생성했다 하더라도 false 를 return 할 수 있음.
                if (!slicent) cout << "created" << endl;
                continue;
            }
            zip.extract(f, dest.u8string());
            if (!slicent) cout << "done" << endl;
        }
    }
    catch (const runtime_error& e)
    {
        cerr << e.what() << endl;
        return false;
    }
    return true;
}

bool ExtractZipToSourceDir(const string& sourceFilePath, bool slicent = false)
{
    if (filesystem::exists(sourceFilePath) == false) return false; // not exist

    const auto& zipFilePath = filesystem::path(sourceFilePath);
    const auto& workingPath = zipFilePath.parent_path();
    return ExtractZip(zipFilePath, workingPath, slicent);
}

string ReadFirstLine(const string& filePath)
{
    ifstream f(filePath);
    string line;
    getline(f, line);
    return line;
}

struct VersionInfo
{
    string Version;
    string ZipFileUrl;
    string ExecutePath;

    JS_OBJ(Version, ZipFileUrl, ExecutePath);

    bool Load(const string& json)
    {
        return LoadFrom(*this, json);
    }
};

bool Execute(const string& versionFilePath)
{
    VersionInfo version;
    const auto& json = ReadTextFrom(versionFilePath);
    if (json.empty()) return false;
    if (version.Load(json) == false) return false;

    stringstream command;
    command << "start " << version.ExecutePath;
    system(command.str().c_str());
    return true;
}

int main(int argc, const char** argv)
{
    const auto& VERSION_FILE_NAME = string(u8"version.json");
    const auto& VERSION_TMP_FILE_NAME = string(VERSION_FILE_NAME + u8".tmp");
    const auto& ZIP_FILE_NAME = string(u8"package.zip");

    Arguments args(argc, argv);

    const auto& appPath = filesystem::path(args.GetParser().Prog());
    auto appFileName = appPath.filename();
    auto appConfigName = appFileName.replace_extension(u8"config");

    // config load
    AppConfig appConfig;
    bool configExists = filesystem::exists(appConfigName);
    if (configExists) appConfig.Load(ReadTextFrom(appConfigName.u8string()));

    if (argc < 2 && configExists == false)
    {
        cout << args.GetParser() << endl;
        return static_cast<int>(AppResult::PARAMETER_ERROR);
    }

    const auto& versionUrl = args.versionUrl.Matched()
        ? args.versionUrl.Get() // override config
        : appConfig.VersionUrl;

    cout << "checking version .. " << versionUrl << endl;
    if (Download(versionUrl, VERSION_TMP_FILE_NAME) == false)
    {
        return static_cast<int>(AppResult::REQUEST_ERROR);
    }

    VersionInfo newVersion;
    const auto& newVersionJson = ReadTextFrom(VERSION_TMP_FILE_NAME);
    if (newVersionJson.empty()) return static_cast<int>(AppResult::VERSION_JOSN_ERROR);
    if (newVersion.Load(newVersionJson) == false) return static_cast<int>(AppResult::VERSION_JOSN_ERROR);

    VersionInfo oldVersion;
    if (filesystem::exists(VERSION_FILE_NAME))
    {
        const auto& oldVersionJson = ReadTextFrom(VERSION_FILE_NAME);
        oldVersion.Load(oldVersionJson);
    }
    else
    {
        cout << "no previous version file. is it first time?" << endl;
    }

    if (newVersion.Version == oldVersion.Version)
    { // no patch needed
        cout << "this version is up-to-date, running " << newVersion.ExecutePath << endl;
        system((string(u8"start ") + newVersion.ExecutePath).c_str()); // run process
        return static_cast<int>(AppResult::OK);
    }

    // download package
    cout << "here comes new version... downloading " << newVersion.ZipFileUrl << endl;
    if (Download(newVersion.ZipFileUrl, ZIP_FILE_NAME) == false)
    {
        return static_cast<int>(AppResult::REQUEST_ERROR);
    }

    // patch
    cout << "unpacking.." << endl;
    if (ExtractZipToSourceDir(ZIP_FILE_NAME) == false)
    {
        return static_cast<int>(AppResult::FILESYSTEM_ERROR);
    }

    // update local version file
    filesystem::remove(VERSION_FILE_NAME);
    filesystem::rename(VERSION_TMP_FILE_NAME, VERSION_FILE_NAME);

    // run
    system((string(u8"start ") + newVersion.ExecutePath).c_str()); // run process
    return static_cast<int>(AppResult::OK);
}