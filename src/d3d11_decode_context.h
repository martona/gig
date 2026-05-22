#pragma once

#include <memory>
#include <mutex>

struct ID3D11Device;

struct D3D11DecodeContext {
    ID3D11Device* device = nullptr;
    std::shared_ptr<std::recursive_mutex> lock;
};
