#include <iostream>
#include <thread>
#include <atomic>
#include "lockfree_queue.h"

int main() {
    LockFreeQueue<int> queue(5);

    std::cout << "Enqueue 1: " << queue.Enqueue(1) << std::endl;
    std::cout << "Enqueue 2: " << queue.Enqueue(2) << std::endl;
    std::cout << "Enqueue 3: " << queue.Enqueue(3) << std::endl;
    std::cout << "Enqueue 4: " << queue.Enqueue(4) << std::endl;
    std::cout << "Enqueue 5: " << queue.Enqueue(5) << std::endl;
    std::cout << "Enqueue 6 should fail): " << queue.Enqueue(6) << std::endl; // Should fail, queue is full

    int val;
    std::cout << "Dequeue: " << queue.Dequeue(val) << " value: "<< val << std::endl;
    std::cout << "Dequeue: " << queue.Dequeue(val) << " value: "<< val << std::endl;
    std::cout << "Dequeue: " << queue.Dequeue(val) << " value: "<< val << std::endl;
    std::cout << "Dequeue: " << queue.Dequeue(val) << " value: "<< val << std::endl;
    std::cout << "Dequeue: " << queue.Dequeue(val) << " value: "<< val << std::endl;
    std::cout << "Dequeue (should fail): " << queue.Dequeue(val) << " value: "<< val << std::endl;

    std::cout << "\n========单线程测试完毕========\n" << std::endl;

    constexpr int N = 10000;
    LockFreeQueue<int> q(1000);

    std::atomic<bool> producer_done(false);
    std::atomic<int> error_count(0);

    //生产者线程
    std::thread producer([&]() {
        for(int i = 0; i < N; i++) {
            while(!q.Enqueue(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });

    //消费者线程
    std::thread consumer([&]() {
        int expected = 0;
        while(expected < N) {
            int val;
            if(q.Dequeue(val)) {
                if(val != expected) {
                    std::cout << "错误：期望 " << expected << ", 实际 " << val << std::endl;
                    error_count.fetch_add(1);
                } 
                expected++;
            }else{
                //队列空，如果有生产者还没结束，稍等再试
                if(!producer_done.load()) {
                    std::this_thread::yield();
                }
            } 
        }
    });

    producer.join();
    consumer.join();

    if(error_count.load() == 0) {
        std::cout << "并发测试通过！" << N << " 个元素无错误." << std::endl;
    }else {
        std::cout << "并发测试失败！ 错误数：" << error_count.load() << std::endl;
    }


}