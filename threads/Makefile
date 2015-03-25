EXECUTABLES = queue

all: $(EXECUTABLES)

.SILENT:

queue: queue.cc
	$(CXX) -o $@ $< -O2 -pthread -std=c++1y

clean:
	@rm -f $(EXECUTABLES)
