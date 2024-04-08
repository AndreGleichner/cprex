# Playground for cpr extensions

See https://github.com/libcpr/cpr

Tries the following:
- Factory to store named sets of standard Session configurations (baseURL, Header, Parameters)
- All Session objects created share DNS, connection, SSL and cookie cache
- Session are configured with a retry policy with backof
- Can invoke verbs (Get, etc) with relative URLs

It provides a class cprex::Session derived from cpr::Session.

No MultiPerform yet.

Basic use:

```cpp
cprex::Session::Factory f;
f.PrepareSession("ipify", "https://api64.ipify.org");
f.PrepareSession("stat", "https://httpstat.us/");
...
auto ipify = f.CreateSession("ipify");
auto r = ipify.Get("/");
...
auto stat = f.CreateSession("stat");
r = stat.Get("/200");
```

Some parameter:
```cpp
r = stat.Get("/200", cpr::Parameters {{"sleep", "5000"}});
```

TODOs:
- maybe eval the response header Retry-After in request retries
- maybe resolve IP in PrepareSession() and also maybe perform connectivity tests to e.g. fallback from proxy to direct
- Add decorrelation jitter as described here:
    - https://github.com/App-vNext/Polly/wiki/Retry-with-jitter
    - https://www.pollydocs.org/strategies/retry
    - https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/tree/master
    - https://github.com/Polly-Contrib/Polly.Contrib.WaitAndRetry/blob/master/src/Polly.Contrib.WaitAndRetry/Backoff.DecorrelatedJitterV2.cs
