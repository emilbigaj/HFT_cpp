#include "Scenario.hpp"
#include "Tools.hpp"

int main()
{
    try
    {
        Strategy::Scenario scenario("Proxy");
        scenario.BuildStrategy();
        scenario.Start();
    }
    catch (const std::exception& e)
    {
        Tools::PrintLine(e.what());
        return 1;
    }
}
