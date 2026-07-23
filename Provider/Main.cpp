#include "Order.hpp"
#include <iostream>

int main()
{
    Execution::RiskLimit riskLimit(1);
    riskLimit.MaxOrderQuantity = 10;
    riskLimit.MaxPositionQuantity = 100;
    riskLimit.MaxOrdersPerSession = Execution::RateLimit(Tools::Duration::FromDays(int64_t{1}), 1'000'000);
    riskLimit.MaxOrdersPerSecond = Execution::RateLimit(Tools::Duration::FromSeconds(int64_t{1}), 300);

    std::string json = Tools::Json::SerializeToLine(riskLimit);
    std::cout << "RiskLimit: " << json << std::endl;

    Execution::RiskLimit riskLimit2 = Tools::Json::Deserialize<Execution::RiskLimit>(json);
    std::cout << "RiskLimit2: " << Tools::Json::Serialize(riskLimit2) << std::endl;

    std::cout << "ServerTests running..." << std::endl;
    return 0;
}