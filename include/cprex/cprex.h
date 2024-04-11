#pragma once
#include <chrono>
#include <thread>
#include <cmath>
#include <variant>

#include <cpr/cpr.h> // https://github.com/libcpr/cpr
#include "proxy.h"   // https://github.com/libproxy/libproxy

namespace cprex
{
static inline bool IsAbsoluteUrl(const std::string& url)
{
    // This of course is not a full check but only a pragmatic approach.
    // It would require a full URL parser like boost::URL.
    return url.starts_with("http:") || url.starts_with("https:");
}

std::string AppendUrls(const std::string& baseUrl, const std::string& otherUrl);

namespace StatusCode
{
bool Succeeded(long statusCode);
bool CanRetry(long statusCode);
}

// Do more resilience like in Polly: https://github.com/App-vNext/Polly
// https://www.pollydocs.org/strategies/retry
using BackofPolicy = std::function<std::chrono::milliseconds(size_t attempt)>;
struct RetryPolicy
{
    // Set maxRetries=0 to not retry at all and thus do only a single request attempt.
    size_t maxRetries;

    // Number of non HTTP errors after which we continue with a single direct (w/o proxy) connection.
    // If that succeeds we remove the proxy configuration for the respective session.
    // Shall be less than to maxRetries.
    // =0 to never fallback to direct.
    size_t directFallbackThreshold;

    // AMount of milliseconds to wait after every request attempt.
    BackofPolicy backofPolicy;
};
extern const BackofPolicy DefaultExponentialBackofPolicy;
extern const RetryPolicy  DefaultRetryPolicy;

// Implementation is identical to cpr::Url and is intended to hold a relative URL,
// typically only the path part of an URL.
class Path : public cpr::StringHolder<Path>
{
public:
    Path() = default;
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    Path(std::string url) : cpr::StringHolder<Path>(std::move(url))
    {
    }
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    Path(std::string_view url) : cpr::StringHolder<Path>(url)
    {
    }
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions)
    Path(const char* url) : cpr::StringHolder<Path>(url)
    {
    }
    Path(const char* str, size_t len) : cpr::StringHolder<Path>(std::string(str, len))
    {
    }
    Path(const std::initializer_list<std::string> args) : cpr::StringHolder<Path>(args)
    {
    }
    Path(const Path& other)   = default;
    Path(Path&& old) noexcept = default;
    ~Path() override          = default;

    Path& operator=(Path&& old) noexcept = default;
    Path& operator=(const Path& other)   = default;
};

class Factory;

class Session
{
    friend Factory;

public:
    void SetRetryPolicy(RetryPolicy retryPolicy)
    {
        _retryPolicy = retryPolicy;
    }

    // During request retries we may try direct even with configured proxy.
    // There we temp. drop configured proxy but may need to restore later.
    void StoreProxy(const std::string& proxy)
    {
        _proxy = proxy;
    }
    void RestoreProxy()
    {
        if (!_proxy.empty())
        {
            const std::string protocol = _url.str().substr(0, _url.str().find(':'));
            _session.SetProxies({{protocol, _proxy}});
        }
    }

    void EnableTrace();

private:
    cpr::Session _session;
    cpr::Url     _url;
    Path         _path;
    std::string  _proxy;

    std::function<void(Session*)>                            _prepper;
    std::function<void(Session*, std::ofstream&)>            _prepperDlStream;
    std::function<void(Session*, const cpr::WriteCallback&)> _prepperDlCallback;

    std::variant<std::ofstream*, const cpr::WriteCallback*> _prepperArgs;

    struct DebugData
    {
        DebugData()
        {
            traceAscii = true;
        }
        bool traceAscii;
    };
    RetryPolicy _retryPolicy;
    DebugData   _debugData;
    static int  curl_trace(CURL* handle, curl_infotype type, char* data, size_t size, void* userp);
    static void dump(const char* text, FILE* stream, unsigned char* ptr, size_t size, bool nohex);

    void SetUrl(const cpr::Url& url)
    {
        _url = url;
        _session.SetUrl(url);
    }

    void          prepare();
    CURLcode      makeRepeatedRequestEx();
    cpr::Response makeRequestEx();
    cpr::Response makeDownloadRequestEx();

    std::chrono::milliseconds ParseRetryAfterHeader();

#ifdef _WIN32
#    pragma region Option setter
#endif
    void SetPath(const Path& path)
    {
        _path = path;
        _session.SetUrl(AppendUrls(std::string(_url), std::string(_path)));
    }

    template <bool processed_header, typename CurrentType>
    void set_option_internal(CurrentType&& current_option)
    {
        static_assert(!std::is_same<CurrentType, cpr::Url>::value,
            "You shall not pass cpr::Url(\"...\"), instead use Path(\"relative/path\"). "
            "Absolute URLs should be passed via Session::Factory::PrepareSession()");

        _session.SetOption(std::forward<CurrentType>(current_option));
    }

    template <>
    void set_option_internal<false, Path>(Path&& current_option)
    {
        SetPath(std::forward<Path>(current_option));
    }

    template <>
    void set_option_internal<true, cpr::Header>(cpr::Header&& current_option)
    {
        // Header option was already provided -> Update previous header
        _session.UpdateHeader(std::forward<cpr::Header>(current_option));
    }

    template <bool processed_header, typename CurrentType, typename... Ts>
    void set_option_internal(CurrentType&& current_option, Ts&&... ts)
    {
        // This was in cpr::Session, but seems useless:
        // set_option_internal<processed_header, CurrentType>(std::forward<CurrentType>(current_option));

        if (std::is_same<CurrentType, cpr::Header>::value)
        {
            set_option_internal<true, Ts...>(std::forward<Ts>(ts)...);
        }
        else
        {
            set_option_internal<processed_header, Ts...>(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    void set_option(Ts&&... ts)
    {
        set_option_internal<false, Ts...>(std::forward<Ts>(ts)...);
    }
#ifdef _WIN32
#    pragma endregion
#endif

public:
#ifdef _WIN32
#    pragma region HTTP verb methods
#endif
    void PrepareDelete();
    void PrepareGet();
    void PrepareHead();
    void PrepareOptions();
    void PreparePatch();
    void PreparePost();
    void PreparePut();
    void PrepareDownload(const cpr::WriteCallback& write);
    void PrepareDownload(std::ofstream& file);

    // Get methods
    template <typename... Ts>
    cpr::Response Get(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PrepareGet;
        return makeRequestEx();
    }

    // Get async methods
    template <typename... Ts>
    cpr::AsyncResponse GetAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Get(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Get callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto GetCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Get(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Post methods
    template <typename... Ts>
    cpr::Response Post(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PreparePost;
        return makeRequestEx();
    }

    // Post async methods
    template <typename... Ts>
    cpr::AsyncResponse PostAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Post(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Post callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto PostCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Post(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Put methods
    template <typename... Ts>
    cpr::Response Put(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PreparePut;
        return makeRequestEx();
    }

    // Put async methods
    template <typename... Ts>
    cpr::AsyncResponse PutAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Put(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Put callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto PutCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Put(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Head methods
    template <typename... Ts>
    cpr::Response Head(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PrepareHead;
        return makeRequestEx();
    }

    // Head async methods
    template <typename... Ts>
    cpr::AsyncResponse HeadAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Head(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Head callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto HeadCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Head(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Delete methods
    template <typename... Ts>
    cpr::Response Delete(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PrepareDelete;
        return makeRequestEx();
    }

    // Delete async methods
    template <typename... Ts>
    cpr::AsyncResponse DeleteAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Delete(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Delete callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto DeleteCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Delete(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Options methods
    template <typename... Ts>
    cpr::Response Options(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PrepareOptions;
        return makeRequestEx();
    }

    // Options async methods
    template <typename... Ts>
    cpr::AsyncResponse OptionsAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Options(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Options callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto OptionsCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Options(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Patch methods
    template <typename... Ts>
    cpr::Response Patch(Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper = &Session::PreparePatch;
        return makeRequestEx();
    }

    // Patch async methods
    template <typename... Ts>
    cpr::AsyncResponse PatchAsync(Ts... ts)
    {
        return cpr::async([](Ts... ts_inner) { return Patch(std::move(ts_inner)...); }, std::move(ts)...);
    }

    // Patch callback methods
    template <typename Then, typename... Ts>
    // NOLINTNEXTLINE(fuchsia-trailing-return)
    auto PatchCallback(Then then, Ts... ts)
    {
        return cpr::async([](Then then_inner, Ts... ts_inner) { return then_inner(Patch(std::move(ts_inner)...)); },
            std::move(then), std::move(ts)...);
    }

    // Download methods
    template <typename... Ts>
    cpr::Response Download(std::ofstream& file, Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper         = nullptr;
        _prepperDlStream = static_cast<void (cpr::Session::*)(std::ofstream&)>(&Session::PrepareDownload);
        _prepperArgs     = &file;
        return makeDownloadRequestEx();
    }

    // Download async method
    template <typename... Ts>
    cpr::AsyncResponse DownloadAsync(cpr::fs::path local_path, Ts... ts)
    {
        return cpr::AsyncWrapper {std::async(
            std::launch::async,
            [](cpr::fs::path local_path_, Ts... ts_) {
                std::ofstream f(local_path_.c_str());
                return Download(f, std::move(ts_)...);
            },
            std::move(local_path), std::move(ts)...)};
    }

    // Download with user callback
    template <typename... Ts>
    cpr::Response Download(const cpr::WriteCallback& write, Ts&&... ts)
    {
        set_option(std::forward<Ts>(ts)...);
        _prepper           = nullptr;
        _prepperDlCallback = static_cast<void (cpr::Session::*)(const cpr::WriteCallback&)>(&Session::PrepareDownload);
        _prepperArgs       = &write;
        return makeDownloadRequestEx();
    }
#ifdef _WIN32
#    pragma endregion
#endif
};

class Factory final
{
    Factory() = delete;

    struct Entry
    {
        Entry();
        ~Entry();

        std::string              name;
        CURLSH*                  share;
        std::string              baseUrl;
        cpr::Header              header;
        cpr::Parameters          parameters;
        cpr::Redirect            redirect;
        RetryPolicy              retryPolicy;
        std::vector<std::string> proxies;
    };
    static std::map<std::string, Entry> _namedSessionsData;
    static pxProxyFactory*              _proxyFactory;
    static std::mutex                   _proxyFactoryMtx;

public:
    static Session CreateSession(const std::string& name, bool trace = false);

    // baseUrl is assumed as an absolute URL as in https://datatracker.ietf.org/doc/html/rfc3986
    static void PrepareSession(const std::string& name, const std::string& baseUrl, const cpr::Header& header = {},
        const cpr::Parameters& parameters = {}, const cpr::Redirect& redirect = {},
        RetryPolicy retryPolicy = DefaultRetryPolicy);

private:
    static bool IsProxyReachable(const std::string& url);
};

}
