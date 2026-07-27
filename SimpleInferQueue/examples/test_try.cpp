#include <iostream>
#include "block_queue.h"

int main() {
    BlockQueue<int> queue(3);

    // 连续 try_Push 三次，都应成功
    std::cout << "try_Push 1: " << queue.try_Push(1) << std::endl;
    std::cout << "try_Push 2: " << queue.try_Push(2) << std::endl;
    std::cout << "try_Push 3: " << queue.try_Push(3) << std::endl;

    // 第四次 try_Push，队列满，应失败
    std::cout << "try_Push 4 (满): " << queue.try_Push(4) << std::endl; 

    // 连续 try_Pop 三次，都应成功
    int value;
    std::cout << "try_Pop: " << queue.try_Pop(value) << " -> " << value << std::endl;
    std::cout << "try_Pop: " << queue.try_Pop(value) << " -> " << value << std::endl;
    std::cout << "try_Pop: " << queue.try_Pop(value) << " -> " << value << std::endl;

    // 第四次 try_Pop，队列空，应失败
    std::cout << "try_Pop (空): " << queue.try_Pop(value) << std::endl;

    return 0;
}
