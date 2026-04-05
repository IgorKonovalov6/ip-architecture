#include <iostream>
#include <cstdlib>
#include <ctime>
#include "motor.h"

int getCloudCoverage() {
    // имитация облачности 0..100%
    return rand() % 101;
}

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    
    std::cout << "Система мониторинга неба" << std::endl;
    
    Motor diesel;
    int cloud = getCloudCoverage();
    
    std::cout << "Облачность: " << cloud << "%" << std::endl;
    
    if (cloud > 70) {
        std::cout << "Высокая облачность, запуск дизеля" << std::endl;
        diesel.startDiesel();
    } else {
        std::cout << "Облачность в норме" << std::endl;
        diesel.stopDiesel();
    }
    
    std::cout << "Завершение" << std::endl;
    return 0;
}