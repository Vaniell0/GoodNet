#include "logger.hpp"

LOGGER("main");

int main(int argc, char* argv[]) {
    logger.info("Start");

    logger.info("Stop");
    return 0;
}