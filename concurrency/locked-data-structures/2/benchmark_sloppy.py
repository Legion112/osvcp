import subprocess
import re
import matplotlib.pyplot as plt
import numpy as np
import os

def compile_c_program():
    """Compile the C program if needed"""
    if not os.path.exists('sloppy_counter'):
        print("Compiling sloppy_counter.c...")
        result = subprocess.run(['gcc', '-o', 'sloppy_counter', 'sloppy_counter.c', '-lpthread', '-lrt'],
                                stderr=subprocess.PIPE)
        if result.returncode != 0:
            print("Compilation failed:")
            print(result.stderr.decode())
            exit(1)

def run_sloppy_counter(num_threads, increments):
    """Run the sloppy counter program and return the time taken"""
    cmd = ['./sloppy_counter', str(num_threads), str(increments // num_threads)]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    if result.returncode != 0:
        print(f"Error running with {num_threads} threads:")
        print(result.stderr.decode())
        return None

    output = result.stdout.decode()

    # Parse the time from output
    time_match = re.search(r'Time taken: (\d+\.\d+) ms', output)
    if not time_match:
        print(f"Could not parse time from output:\n{output}")
        return None

    return float(time_match.group(1))

def main():
    # Get user input
    min_threads = int(input("Enter minimum number of threads: "))
    max_threads = int(input("Enter maximum number of threads: "))
    increments = int(input("Enter number of increments per thread: "))

    if min_threads < 1 or max_threads < min_threads:
        print("Invalid thread range")
        return

    if increments < 1:
        print("Increments must be positive")
        return

    compile_c_program()

    # Test different thread counts
    thread_counts = range(min_threads, max_threads + 1)
    times = []

    print("\nRunning benchmarks...")
    for num_threads in thread_counts:
        print(f"Testing with {num_threads} threads...", end=' ', flush=True)
        time_taken = run_sloppy_counter(num_threads, increments)
        if time_taken is not None:
            times.append(time_taken)
            print(f"Time: {time_taken:.2f} ms")
        else:
            print("Failed")
            return

    # Plot results
    plt.figure(figsize=(10, 6))
    plt.plot(thread_counts, times, 'bo-')
    plt.title(f'Time vs Number of Threads ({increments} increments per thread)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Time (ms)')
    plt.grid(True)

    # Annotate points
    for i, (threads, time) in enumerate(zip(thread_counts, times)):
        plt.annotate(f'{time:.1f}ms',
                     (threads, time),
                     textcoords="offset points",
                     xytext=(0,10),
                     ha='center')

    plt.xticks(thread_counts)
    plt.tight_layout()

    # Save and show plot
    plot_filename = f'sloppy_counter_{min_threads}-{max_threads}threads_{increments}inc.png'
    plt.savefig(plot_filename)
    print(f"\nPlot saved as {plot_filename}")
    plt.show()

if __name__ == "__main__":
    main()