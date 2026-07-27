#include <thread>
#include <chrono>
#include <iostream>
#include "block_queue.h"

int main(){
    BlockQueue<int> queue(5);

    std::thread producer([&queue](){
        for(int i = 0; i < 20; i++){
            queue.Push(i);
            std::cout << "生产了：" << i << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::thread consumer([&queue](){
        for(int i = 0; i < 20; i++){
            int value = queue.Pop();
            std::cout << "消费了：" << value << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });

    producer.join();
    consumer.join();
    std::cout << "测试结束" << std::endl;
    return 0;
}