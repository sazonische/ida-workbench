#pragma once

#include <concepts>
#include <type_traits>

template <typename T>
	requires std::is_class_v<T>
class Singleton {
public:
	Singleton(const Singleton&) = delete;
	Singleton& operator=(const Singleton&) = delete;
	Singleton(Singleton&&) = delete;
	Singleton& operator=(Singleton&&) = delete;

	[[nodiscard]] static T& Instance() noexcept(std::is_nothrow_constructible_v<T>) {
		static T instance;
		return instance;
	}

	[[nodiscard]] static T* InstancePtr() {
		static auto* instance = new T;
		return instance;
	}

protected:
	Singleton() = default;
	~Singleton() = default;

	friend T;
};
