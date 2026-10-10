#include "block_queue.h"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    BlockQueue<int> queue(5);
    constexpr int kTotal = 20;

    std::thread producer([&queue, kTotal]() {
        for (int i = 0; i < kTotal; i++) {
            queue.Push(i);
            std::cout << "生产了：" << i << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // 生产结束，关闭队列——这是消费者唯一能知道「不会再有数据了」的信号
        queue.Close();
        std::cout << "生产者结束，队列已关闭" << std::endl;
    });

    std::thread consumer([&queue]() {
        int value = 0;
        // 不再靠「固定循环 20 次」来收尾：那样只是在赌生产端一定跑在前面，
        // 一旦时序变化消费者就会永远阻塞在 Pop 上。改成看 Pop 的返回值——
        // 它返回 false 当且仅当队列已关闭且已取空
        while (queue.Pop(value)) {
            std::cout << "消费了：" << value << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        std::cout << "队列已关闭且已取空，消费者退出" << std::endl;
    });

    producer.join();
    consumer.join();
    std::cout << "测试结束" << std::endl;
    return 0;
}
