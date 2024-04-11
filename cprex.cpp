#include <iostream>

#include "include/cprex/cprex.h"
using namespace std::chrono_literals;

namespace cprex
{
std::string AppendUrls(const std::string& baseUrl, const std::string& otherUrl)
{
    std::string res;
    if (otherUrl.empty())
    {
        res = baseUrl;
    }
    else
    {
        if (IsAbsoluteUrl(otherUrl))
        {
            res = otherUrl;
        }
        else
        {
            if (otherUrl.front() == '/')
                res = baseUrl + otherUrl.substr(1);
            else
                res = baseUrl + otherUrl;
        }
    }
    return res;
}

namespace StatusCode
{
bool Succeeded(long statusCode)
{
    return statusCode >= 200 && statusCode <= 299;
}
bool CanRetry(long statusCode)
{
    // “Non-retryable” status codes are:
    // * NotModified(304)
    // * all 400( >= 400 and < 500) class exceptions (Bad gateway, Not Found, etc.), except RequestTimeout(408)
    // * NotImplemented(501) and HttpVersionNotSupported(505).

    // statusCode=0 means the server has not send a response because e.g. we couldn't resolve, connect, etc

    return statusCode == 0 || (statusCode < 400 && statusCode != 304 /*NotModified*/) ||
           statusCode == 408 /*RequestTimeout*/ ||
           (statusCode >= 500 && statusCode != 501 /*NotImplemented*/ && statusCode != 505 /*HttpVersionNotSupported*/);
}
}

const BackofPolicy DefaultExponentialBackofPolicy = [](size_t attempt) {
    // TODO add decorrelation jitter:
    // https://github.com/App-vNext/Polly/wiki/Retry-with-jitter
    // https://www.pollydocs.org/strategies/retry
    // https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/tree/master
    // https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/blob/master/src/Polly.Contrib.WaitAndRetry/Backoff.DecorrelatedJitterV2.cs

    if (attempt > 12)
        return std::chrono::milliseconds(10min);

    size_t milliSeconds = 100 << attempt;

    return std::chrono::milliseconds(milliSeconds);
};

const RetryPolicy DefaultRetryPolicy {5, 4, DefaultExponentialBackofPolicy};

void Session::EnableTrace()
{
    CURL* curl = _session.GetCurlHolder()->handle;

    // https://curl.se/libcurl/c/debug.html

    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_trace);
    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &_debugData);

    // the DEBUGFUNCTION has no effect until we enable VERBOSE
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
}

void Session::dump(const char* text, FILE* stream, unsigned char* ptr, size_t size, bool nohex)
{
    size_t i;
    size_t c;

    unsigned int width = 0x10;

    if (nohex)
        /* without the hex output, we can fit more on screen */
        width = 0x40;

    fprintf(stream, "%s, %10.10lu bytes (0x%8.8lx)\n", text, (unsigned long)size, (unsigned long)size);

    for (i = 0; i < size; i += width)
    {
        fprintf(stream, "%4.4lx: ", (unsigned long)i);

        if (!nohex)
        {
            /* hex not disabled, show it */
            for (c = 0; c < width; c++)
                if (i + c < size)
                    fprintf(stream, "%02x ", ptr[i + c]);
                else
                    fputs("   ", stream);
        }

        for (c = 0; (c < width) && (i + c < size); c++)
        {
            /* check for 0D0A; if found, skip past and start a new line of output */
            if (nohex && (i + c + 1 < size) && ptr[i + c] == 0x0D && ptr[i + c + 1] == 0x0A)
            {
                i += (c + 2 - width);
                break;
            }
            fprintf(stream, "%c", (ptr[i + c] >= 0x20) && (ptr[i + c] < 0x80) ? ptr[i + c] : '.');
            /* check again for 0D0A, to avoid an extra \n if it's at width */
            if (nohex && (i + c + 2 < size) && ptr[i + c + 1] == 0x0D && ptr[i + c + 2] == 0x0A)
            {
                i += (c + 3 - width);
                break;
            }
        }
        fputc('\n', stream); /* newline */
    }
    fflush(stream);
}

int Session::curl_trace(CURL* handle, curl_infotype type, char* data, size_t size, void* userp)
{
    auto        config = (DebugData*)userp;
    const char* text;
    (void)handle; /* prevent compiler warning */

    switch (type)
    {
        case CURLINFO_TEXT:
            fprintf(stderr, "== Info: %s", data);
            return 0;
        case CURLINFO_HEADER_OUT:
            text = "=> Send header";
            break;
        case CURLINFO_DATA_OUT:
            text = "=> Send data";
            break;
        case CURLINFO_SSL_DATA_OUT:
            text = "=> Send SSL data";
            break;
        case CURLINFO_HEADER_IN:
            text = "<= Recv header";
            break;
        case CURLINFO_DATA_IN:
            text = "<= Recv data";
            break;
        case CURLINFO_SSL_DATA_IN:
            text = "<= Recv SSL data";
            break;
        default: /* in case a new one is introduced to shock us */
            return 0;
    }

    dump(text, stderr, (unsigned char*)data, size, config->traceAscii);
    return 0;
}

CURLcode Session::makeRepeatedRequestEx()
{
    CURL*    curl = _session.GetCurlHolder()->handle;
    CURLcode curl_error;
    size_t   attempt           = 0;
    size_t   nonHttpErrors     = 0;
    bool     tempProxyDisabled = false;
    bool     keepProxyDisabled = false;

    while (1)
    {
        prepare();

        curl_error = curl_easy_perform(curl);
        if (curl_error != CURLE_OK)
            ++nonHttpErrors;

        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

        if (StatusCode::Succeeded(status_code))
        {
            // std::cout << "    Success(" << status_code << "): " << std::endl;
            if (tempProxyDisabled)
                keepProxyDisabled = true;
            break;
        }

        if (!StatusCode::CanRetry(status_code))
        {
            std::cout << "    Can't retry(" << status_code << "): " << std::endl;
            break;
        }

        if (attempt >= _retryPolicy.maxRetries)
        {
            std::cout << "    Failed and can't retry any more" << std::endl;
            break;
        }

        // Check whether there's a Retry-After header
        auto waitMilliSeconds = ParseRetryAfterHeader();
        if (waitMilliSeconds == 0ms)
            waitMilliSeconds = _retryPolicy.backofPolicy(attempt++);

        std::cout << "    Failed (" << attempt << ") with " << status_code << ", retry after " << waitMilliSeconds
                  << " ... " << std::endl;

        // In proxied request case if we have enabled a fallback to direct and there were enough attempts w/o any
        // response from server.
        if (_retryPolicy.directFallbackThreshold > 0 && nonHttpErrors > _retryPolicy.directFallbackThreshold)
        {
            // Temp disable proxy
            _session.SetProxies({{}});
            tempProxyDisabled = true;
        }

        std::this_thread::sleep_for(waitMilliSeconds);
    };

    if (tempProxyDisabled && !keepProxyDisabled)
    {
        RestoreProxy();
    }

    return curl_error;
}

static std::time_t HttpDate(const char* v)
{
    std::tm            tm = {};
    std::istringstream ss(v);
    ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");

    if (ss.fail())
        return -1;

    std::time_t date = std::mktime(&tm);
    if (date == -1)
        return -1;

    return date;
}

std::chrono::milliseconds Session::ParseRetryAfterHeader()
{
    CURL* curl = _session.GetCurlHolder()->handle;

    curl_header* header      = nullptr;
    long long    retry_after = 0;
    if (CURLHE_OK == curl_easy_header(curl, "Retry-After", 0, CURLH_HEADER, -1, &header))
    {
        // Could be an integer value (Seconds) or a HTTP-date
        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Retry-After

        try
        {
            retry_after = std::stoll(header->value);
        }
        catch (const std::exception&)
        {
        }
        if (!retry_after)
        {
            auto date = HttpDate(header->value);
            if (-1 != date)
                // convert date to number of seconds into the future
                retry_after = date - time(NULL);
        }
    }

    if (retry_after)
        return retry_after * 1000ms;
    else
        return 0ms;
}

void Session::prepare()
{
    if (_prepper)
    {
        _prepper(this);
    }
    else
    {
        if (_prepperArgs.index() == 0)
        {
            auto file = std::get<0>(_prepperArgs);
            _prepperDlStream(this, *file);
        }
        else
        {
            auto write = std::get<1>(_prepperArgs);
            _prepperDlCallback(this, *write);
        }
    }
}

cpr::Response Session::makeRequestEx()
{
    return _session.Complete(makeRepeatedRequestEx());
}

cpr::Response Session::makeDownloadRequestEx()
{
    return _session.CompleteDownload(makeRepeatedRequestEx());
}

void Session::PrepareDelete()
{
    _session.PrepareDelete();
}

void Session::PrepareGet()
{
    _session.PrepareGet();
}

void Session::PrepareHead()
{
    _session.PrepareHead();
}

void Session::PrepareOptions()
{
    _session.PrepareOptions();
}

void Session::PreparePatch()
{
    _session.PreparePatch();
}

void Session::PreparePost()
{
    _session.PreparePost();
}

void Session::PreparePut()
{
    _session.PreparePut();
}

void Session::PrepareDownload(std::ofstream& file)
{
    _session.PrepareDownload(file);
}

void Session::PrepareDownload(const cpr::WriteCallback& write)
{
    _session.PrepareDownload(write);
}


std::map<std::string, Factory::Entry> Factory::_namedSessionsData;
pxProxyFactory*                       Factory::_proxyFactory = nullptr;
std::mutex                            Factory::_proxyFactoryMtx;

Factory::Entry::Entry()
{
    // https://everything.curl.dev/helpers/sharing.html
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

Factory::Entry::~Entry()
{
    curl_share_cleanup(share);
}

Session Factory::CreateSession(const std::string& name, bool trace)
{
    auto entry = _namedSessionsData.find(name);
    if (entry == std::end(_namedSessionsData))
        throw new std::exception("CreateNamedSession can't find name");

    auto data = entry->second;

    Session session;
    session.SetUrl(data.baseUrl);
    session._session.SetHeader(data.header);
    session._session.SetParameters(data.parameters);
    session._session.SetRedirect(data.redirect);
    session.SetRetryPolicy(data.retryPolicy);

    if (!data.proxies.empty())
    {
        // Find a reachable proxy, if there is none we automatically do direct requests.
        std::string              proxy;
        std::vector<std::string> proxies {data.proxies};
        while (!proxies.empty())
        {
            size_t index = 0;
            if (proxies.size() > 1)
            {
                // TODO maybe add config option to always only use the 1st proxy for better connection pooling
                srand((unsigned)time(nullptr));
                index = (rand() % proxies.size());
            }
            proxy = proxies[index];
            proxies.erase(proxies.begin() + index);

            if (IsProxyReachable(proxy))
                break;
            else
                proxy.clear();
        }

        if (!proxy.empty())
        {
            // All this URL "parsing" would be much simpler via boost::URL, but maybe too heavy
            const std::string protocol = data.baseUrl.substr(0, data.baseUrl.find(':'));

            size_t ampPos = proxy.find('@');
            if (ampPos != std::string::npos)
            {
                size_t schemaPos        = proxy.find("://");
                auto   proxyWithoutAuth = proxy;
                proxyWithoutAuth.erase(schemaPos + 3, ampPos - schemaPos - 2);
                auto   auth       = proxy.substr(schemaPos + 3, ampPos - schemaPos - 3);
                size_t authColPos = auth.find(':');
                if (authColPos != std::string::npos)
                {
                    auto user = auth.substr(0, authColPos);
                    auto pass = auth.substr(authColPos + 1);
                    session._session.SetProxyAuth(
                        cpr::ProxyAuthentication {{protocol, cpr::EncodedAuthentication {user, pass}}});
                }
            }

            session._session.SetProxies({{protocol, proxy}});
            session.StoreProxy(proxy);
        }
        else
        {
            session._retryPolicy.directFallbackThreshold = 0;
        }
    }

    CURL* curl = session._session.GetCurlHolder()->handle;
    curl_easy_setopt(curl, CURLOPT_SHARE, data.share);

    if (trace)
    {
        session.EnableTrace();
    }

    return session;
}

// baseUrl is assumed as an absolute URL as in https://datatracker.ietf.org/doc/html/rfc3986
void Factory::PrepareSession(const std::string& name, const std::string& baseUrl, const cpr::Header& header,
    const cpr::Parameters& parameters, const cpr::Redirect& redirect, RetryPolicy retryPolicy)
{
    if (!IsAbsoluteUrl(baseUrl))
        throw new std::exception("baseUrl shall be absolute (start with http: or https:)");

    Entry entry;
    entry.name = name;

    if (baseUrl.back() == '/')
        entry.baseUrl = baseUrl;
    else
        entry.baseUrl = baseUrl + '/';

    // TODO maybe resolve here and also maybe perform connectivity tests

    entry.header      = header;
    entry.parameters  = parameters;
    entry.redirect    = redirect;
    entry.retryPolicy = retryPolicy;
    if (entry.retryPolicy.directFallbackThreshold >= entry.retryPolicy.maxRetries && entry.retryPolicy.maxRetries > 0)
        entry.retryPolicy.directFallbackThreshold = entry.retryPolicy.maxRetries - 1;


    {
        std::lock_guard<std::mutex> lock(_proxyFactoryMtx);
        if (!_proxyFactory)
            _proxyFactory = px_proxy_factory_new();

        // TODO This is blocking thus maybe call in another thread and wait on the first CreateSession or first actual
        // request.
        auto   proxies = px_proxy_factory_get_proxies(_proxyFactory, baseUrl.c_str());
        char** proxy   = proxies;
        while (*proxy)
        {
            // Usually one of:
            // direct://
            // http://[username:password@]proxy:port
            if (IsAbsoluteUrl(*proxy))
            {
                entry.proxies.push_back(*proxy);
            }
            ++proxy;
        }

        px_proxy_factory_free_proxies(proxies);
    }

    _namedSessionsData[name] = entry;
}

bool Factory::IsProxyReachable(const std::string& url)
{
    auto r = cpr::Head(cpr::Url(url), cpr::Timeout(1s));
    // if there's any status_code it means the server somehow replied, most probably with 400 Bad Request, as HEAD may
    // not be supported. Usually when a server isn't reachable we get some r.error
    return r.status_code != 0;
}

}
