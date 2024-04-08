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

    return (statusCode < 400 && statusCode != 304 /*NotModified*/) || statusCode == 408 /*RequestTimeout*/ ||
           (statusCode >= 500 && statusCode != 501 /*NotImplemented*/ && statusCode != 505 /*HttpVersionNotSupported*/);
}
}

const Session::BackofPolicy Session::DefaultExponentialBackofPolicy = [](int attempt) {
    // TODO add decorrelation jitter:
    // https://github.com/App-vNext/Polly/wiki/Retry-with-jitter
    // https://www.pollydocs.org/strategies/retry
    // https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/tree/master
    // https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/blob/master/src/Polly.Contrib.WaitAndRetry/Backoff.DecorrelatedJitterV2.cs

    if (attempt > 12)
        return std::chrono::milliseconds(10min);

    __int64 milliSeconds = 100 << attempt;

    return std::chrono::milliseconds(milliSeconds);
};

const Session::RetryPolicy Session::DefaultRetryPolicy {5, DefaultExponentialBackofPolicy};

std::map<std::string, Session::Factory::Entry> Session::Factory::_namedSessionsData;

Session::Factory::Entry::Entry()
{
    // https://everything.curl.dev/helpers/sharing.html
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

Session::Factory::Entry::~Entry()
{
    curl_share_cleanup(share);
}

Session Session::Factory::CreateSession(const std::string& name)
{
    auto entry = _namedSessionsData.find(name);
    if (entry == std::end(_namedSessionsData))
        throw new std::exception("CreateNamedSession can't find name");

    auto data = entry->second;

    Session session;
    session.SetUrl(data.baseUrl);
    session.SetHeader(data.header);
    session.SetParameters(data.parameters);
    session.SetRetryPolicy(data.retryPolicy);

    CURL* curl = session.GetCurlHolder()->handle;
    curl_easy_setopt(curl, CURLOPT_SHARE, data.share);

    return session;
}

// baseUrl is assumed as an absolute URL as in
// https://datatracker.ietf.org/doc/html/rfc3986
void Session::Factory::PrepareSession(const std::string& name, const std::string& baseUrl, const cpr::Header& header,
    const cpr::Parameters& parameters, RetryPolicy retryPolicy)
{
    if (!IsAbsoluteUrl(baseUrl))
        throw new std::exception("baseUrl shall be absolute (start with http: or https:)");

    Entry entry;
    entry.name = name;

    if (baseUrl.back() == '/')
        entry.baseUrl = baseUrl;
    else
        entry.baseUrl = baseUrl + '/';

    // TODO maybe resolve here and also maybe perform connectivity tests to e.g. fallback from proxy to direct

    entry.header      = header;
    entry.parameters  = parameters;
    entry.retryPolicy = retryPolicy;

    _namedSessionsData[name] = entry;
}

CURLcode Session::makeRepeatedRequestEx()
{
    CURL*    curl = GetCurlHolder()->handle;
    CURLcode curl_error;
    int      attempt = 0;

    while (1)
    {
        prepare();

        curl_error = curl_easy_perform(curl);

        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

        if (StatusCode::Succeeded(status_code))
        {
            // std::cout << "    Success(" << status_code << "): " << std::endl;
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

        auto waitMilliSeconds = _retryPolicy.backofPolicy(attempt++);

        // TODO maybe eval the response header Retry-After

        std::cout << "    Failed (" << attempt << ") with " << status_code << ", retry after " << waitMilliSeconds
                  << " ... " << std::endl;

        std::this_thread::sleep_for(waitMilliSeconds);
    };

    return curl_error;
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
    return Complete(makeRepeatedRequestEx());
}

cpr::Response Session::makeDownloadRequestEx()
{
    return CompleteDownload(makeRepeatedRequestEx());
}

}
