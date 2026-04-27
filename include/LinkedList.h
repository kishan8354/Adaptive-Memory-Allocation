#pragma once
#include <cstddef>

// ── Singly-linked intrusive list (original pool/free-list style) ─────────────
struct Node {
    struct {
        std::size_t blockSize = 0;
    } data;
    Node* next = nullptr;
};

struct SinglyLinkedList {
    Node* head = nullptr;

    void insert(Node* previousNode, Node* newNode) {
        if (previousNode == nullptr) {
            if (head != nullptr) newNode->next = head;
            else                 newNode->next = nullptr;
            head = newNode;
        } else {
            if (previousNode->next == nullptr) {
                previousNode->next = newNode;
                newNode->next      = nullptr;
            } else {
                newNode->next      = previousNode->next;
                previousNode->next = newNode;
            }
        }
    }

    void remove(Node* previousNode, Node* deleteNode) {
        if (previousNode == nullptr) {
            head = deleteNode->next;
        } else {
            previousNode->next = deleteNode->next;
        }
    }
};

// ── Stack-linked list (pool allocator) ──────────────────────────────────────
struct StackLinkedList {
    struct Node { Node* next = nullptr; };
    Node* head = nullptr;
    void  push(Node* node) { node->next = head; head = node; }
    Node* pop()            { Node* top = head; if (head) head = head->next; return top; }
};
