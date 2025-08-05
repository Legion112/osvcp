> Build a version of a linked list that uses hand-over-hand locking, as cited in the chapter. You should read the paper first to understand how it works, and then implement it. Measure its performance. When does a hand-over-hand list work better than a standard list as shown in the chapter?

Finish reading on "hand-over-hand locking" https://people.csail.mit.edu/shanir/publications/concurrent-data-structures.pdf

sloppy_counter_only_gloval_lock - имеет оптимизацию нет локальных локав и вся инфа находиться на стеке
Но нельзя в произвольный момент времени получить точное значение count
так как инфа находиться на стеке у каждой Thead 
Можно теоритически получить не точную инфу но без гарантий точности