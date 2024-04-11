# Playground for cpr extensions

See https://github.com/libcpr/cpr

Tries the following:
- Factory to store named sets of standard Session configurations (baseURL, Header, Parameters, Redirects, HTTP proxies)
- All Session objects created share DNS, connection, SSL and cookie cache
- Sessions are configured with a retry policy with backof
- Can invoke verbs (Get, etc) with relative URLs, otherwise same parameters as cpr
- Proxy autodiscovery via libproxy
- proxy connectivity test on session creation
- during request retries optionally try connects w/o proxy

It provides a class cprex::Session utilizing cpr::Session.

No MultiPerform yet.

Basic use:

```cpp
cprex::Factory::PrepareSession("ipify", "https://api64.ipify.org");
cprex::Factory::PrepareSession("stat", "https://httpstat.us/");
...
auto ipify = cprex::Factory::CreateSession("ipify");
auto r = ipify.Get("/");
...
auto stat = cprex::Factory::CreateSession("stat");
r = stat.Get("/200");
```

Some parameter:
```cpp
r = stat.Get("/200", cpr::Parameters {{"sleep", "5000"}});
```

TODOs:
- maybe resolve IP in PrepareSession() and also maybe perform connectivity tests
- Add decorrelation jitter as described here:
    - https://github.com/App-vNext/Polly/wiki/Retry-with-jitter
    - https://www.pollydocs.org/strategies/retry
    - https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/tree/master
    - https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/blob/master/src/Polly.Contrib.WaitAndRetry/Backoff.DecorrelatedJitterV2.cs
