#include <iostream>

#include "include/cprex/cprex.h"

void print(cpr::Response r)
{
    if (cprex::StatusCode::Succeeded(r.status_code))
        std::cout << "Response (" << r.status_code << "): '" << r.text << "'" << std::endl;
    else
        std::cout << "Response (" << r.status_code << "): Request failed" << std::endl;
}

int main()
{
    cprex::Session::Factory::PrepareSession("ipify", "https://api64.ipify.org");

    // https://www.httpbin.org/

    // https://httpstat.us/
    // https://httpstat.us/200
    // https://httpstat.us/Random/200,201,500-504

    cprex::Session::Factory::PrepareSession("rnd", "https://httpstat.us/Random/200,201,502-504");
    cprex::Session::Factory::PrepareSession("stat", "https://httpstat.us/");

    {
        std::cout << "ipify ######################" << std::endl;
        auto ipify = cprex::Session::Factory::CreateSession("ipify");

        // compile error (intentional), shall have a Path object
        // auto r = ipify.Get();
        // print(r);

        auto r = ipify.Get(cprex::Path("/"));
        print(r);

        // compile error (intentional), must not use absolute URL
        // r = ipify.Get(cpr::Url("https://api64.ipify.org"));
        // print(r);

        r = ipify.Get(cprex::Path("/"));
        print(r);

        r = ipify.Get("/");
        print(r);
    }

    {
        std::cout << "rnd ######################" << std::endl;
        auto rnd = cprex::Session::Factory::CreateSession("rnd");
        for (int i = 0; i < 10; ++i)
        {
            std::cout << i << ": ";
            auto r = rnd.Get("/");
            print(r);
        }
    }

    {
        std::cout << "stat ######################" << std::endl;
        auto stat = cprex::Session::Factory::CreateSession("stat");
        auto r    = stat.Get(cprex::Path("/200"));
        print(r);

        r = stat.Get(cprex::Path("/201"));
        print(r);

        r = stat.Get(cprex::Path("/200"), cpr::Parameters {{"sleep", "5000"}});
        print(r);

        r = stat.Get("/200", cpr::Parameters {{"sleep", "5000"}});
        print(r);
    }
}
