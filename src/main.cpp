#include "logger.hpp"
#include "config.hpp"

LOGGER("main");

int main(int argc, char* argv[]) {
    Config conf;
    logger.info("Start");

    logger.info("Stop");
    return 0;
}