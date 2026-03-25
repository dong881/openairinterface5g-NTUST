CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
LDFLAGS = -lpthread

# Source files
MULTISTREAM_SOURCES = nrLDPC_coding_aal_multistream.c test_acc100_multistream.c test_stubs.c
INTEGRATION_SOURCES = nrLDPC_coding_aal_integration.c test_acc100_multistream.c test_stubs.c
OBJECTS = $(MULTISTREAM_SOURCES:.c=.o)
INTEGRATION_OBJECTS = $(INTEGRATION_SOURCES:.c=.o)
TARGET = test_acc100_multistream
INTEGRATION_TARGET = test_integration

# Build rules
all: $(TARGET)

integration: $(INTEGRATION_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

$(INTEGRATION_TARGET): $(INTEGRATION_OBJECTS)
	$(CC) $(INTEGRATION_OBJECTS) -o $(INTEGRATION_TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Test multistream functionality
test: $(TARGET)
	@echo "Running ACC100 multi-stream test..."
	./$(TARGET)

# Test integration layer
test-integration: integration
	@echo "Running ACC100 integration test..."
	./$(INTEGRATION_TARGET)

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(INTEGRATION_OBJECTS) $(TARGET) $(INTEGRATION_TARGET)

.PHONY: all integration test test-integration clean