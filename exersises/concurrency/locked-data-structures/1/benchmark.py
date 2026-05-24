import matplotlib.pyplot as plt
import re
import subprocess
import sys

def run_benchmark(threads, increments):
    """Запускает бенчмарк и возвращает время выполнения"""
    try:
        # Запускаем программу с заданными параметрами
        result = subprocess.run(
            ["/home/legion/github/osvcp/timer", str(threads), str(increments)],
            capture_output=True,
            text=True,
            check=True
        )

        # Парсим время из вывода
        time_match = re.search(r'Elapsed time: (\d+\.\d+) seconds', result.stdout)
        if time_match:
            return float(time_match.group(1))
        else:
            raise ValueError("Не удалось найти время в выводе")

    except subprocess.CalledProcessError as e:
        print(f"Ошибка при выполнении программы: {e}")
        return None

def main():
    if len(sys.argv) != 3:
        print("Использование: python script.py <min_threads> <max_threads>")
        sys.exit(1)

    min_threads = int(sys.argv[1])
    max_threads = int(sys.argv[2])
    increments = 100_000_000  # Количество инкрементов на поток

    threads_count = []
    execution_times = []

    print("Запуск тестов...")

    for threads in range(min_threads, max_threads + 1):
        print(f"Тестируем {threads} потоков...")
        time = run_benchmark(threads, increments)
        if time is not None:
            threads_count.append(threads)
            execution_times.append(time)

    # Построение графика
    plt.figure(figsize=(10, 6))
    plt.plot(threads_count, execution_times, marker='o', linestyle='-')
    plt.title('Зависимость времени выполнения от количества потоков')
    plt.xlabel('Количество потоков')
    plt.ylabel('Время выполнения (секунды)')
    plt.grid(True)
    plt.tight_layout()
    plt.savefig('benchmark_results.png')
    plt.show()

if __name__ == "__main__":
    main()
