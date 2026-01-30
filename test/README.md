# Unit Tests for Message Decoder

This directory contains unit tests for the message decoder module.

## Test Structure

- `test_message_decoder.c` - Unit tests for the message decoder module
  - Checksum verification tests
  - Message decoding tests (mode, temperature, heater, etc.)
  - Malformed message handling tests
  - Helper function tests

## Running Tests

### Option 1: Host-based Testing (No Hardware Required)

Compile and run the tests on your development machine:

```bash
# Navigate to the test directory
cd test

# Compile the tests
gcc -I. -I.. -o test_decoder \
    test_message_decoder.c \
    ../main/message_decoder.c

# Run the tests
./test_decoder
```

The test directory includes mock headers for FreeRTOS and ESP-IDF logging that allow compilation without the full ESP-IDF toolchain.

### Option 2: ESP-IDF Unity Framework

For integration with ESP-IDF's test framework:

```bash
idf.py test
```

## Test Coverage

The tests cover:

1. **Checksum Verification**
   - Valid checksums pass
   - Invalid checksums fail
   - Messages that are too short are rejected

2. **Message Decoding**
   - Mode messages (Spa/Pool)
   - Temperature settings
   - Heater status
   - Chlorinator pH/ORP (extended tests can be added)

3. **Error Handling**
   - Malformed messages (wrong start/end bytes)
   - Short messages
   - Unknown message types

4. **Helper Functions**
   - Device name lookup
   - Gateway status text lookup

## Adding New Tests

To add a new test:

1. Create a test function following the pattern:
```c
void test_my_feature(void)
{
    // Setup
    memset(&test_pool_state, 0, sizeof(test_pool_state));
    test_ctx.pool_state = &test_pool_state;
    test_ctx.enable_mqtt = false;

    // Create test message
    uint8_t msg[] = { /* your message bytes */ };

    // Execute
    bool result = decode_message(msg, sizeof(msg), &test_ctx);

    // Assert
    TEST_ASSERT(result, "Message should be decoded");
    TEST_ASSERT(test_pool_state.some_field == expected_value, "Field should match");
}
```

2. Call your test function from `main()`:
```c
test_my_feature();
```

## Mocking

The tests mock FreeRTOS and MQTT functions to enable testing without hardware:

- `xTaskGetTickCount()` - Returns a fixed value
- `xSemaphoreTake()`/`xSemaphoreGive()` - No-ops that always succeed
- `mqtt_publish_*()` - Empty stubs (MQTT disabled via `enable_mqtt = false`)

## Test Output

Example output:

```
======================================
  Message Decoder Unit Tests
======================================

--- Checksum Tests ---
✓ PASS: Valid checksum should pass verification
✓ PASS: Invalid checksum should fail verification
✓ PASS: Message too short should fail verification

--- Message Decoding Tests ---
✓ PASS: Mode message should be decoded
✓ PASS: Mode should be set to Spa (0)
...

======================================
  Test Summary
======================================
  Passed: 15
  Failed: 0
======================================
```

## Continuous Integration

These tests can be integrated into CI/CD pipelines for automated testing on every commit.
