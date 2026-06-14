#write all members of 6! to a file, one per line, in lexicographic order.
#The file should be named "6_factorial.txt".
from itertools import permutations

for i in range(2):
    for perm in sorted(permutations('1234567890')):
        print(''.join(perm))
