#include <emscripten.h>
#include <iostream>

EM_ASYNC_JS(int, fetch_data, (), {
    const response = await fetch('http://localhost:30000/teapot');
    const text = await response.text();
    console.log(text);
    return response.status;
});

int main() {
    std::cout << "Status: " << fetch_data() << std::endl;
    return 0;
}
