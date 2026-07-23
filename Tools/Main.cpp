#include <iostream>
#include "Timestamp.hpp" // Include your own header

int main()
{
    auto now = Tools::Timestamp::UtcNow();
    std::cout << "Time is: " << now.ToString() << std::endl;
    return 0;
}