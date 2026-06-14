
examples_dir = 'examples'

with open(f'{examples_dir}/large_code.py', 'w+') as fp:
    for i in range(108382801):
        fp.write(f'print("Line {i}")\n')
