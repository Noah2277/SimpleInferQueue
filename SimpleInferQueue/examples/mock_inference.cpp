#include "lockfree_queue.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>

int GenerateRandomNumber() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    thread_local std::uniform_int_distribution<> dis(1000, 9999);
    return dis(gen);
}

struct InferenceRequest {
    int id;
    std::string path;
};

int main() {
    std::atomic<bool> producer_done(false);
    LockFreeQueue<InferenceRequest> queue(10);

    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < 20; i++) {
            int randomId = GenerateRandomNumber();
            InferenceRequest req{randomId, "image/" + std::to_string(randomId) + ".jpg"};
            while (!queue.Enqueue(req)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });

    // 消费者线程
    std::thread consumer([&]() {
        while (true) {
            InferenceRequest req;
            if (queue.Dequeue(req)) {
                std::cout << "Processing request ID: " << req.id << ", Path: " << req.path
                          << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                if (producer_done.load() && queue.Empty()) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    return 0;
}