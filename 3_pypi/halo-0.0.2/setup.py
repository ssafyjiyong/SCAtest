from setuptools import setup, find_packages # pylint: disable=no-name-in-module,import-error

def readme():
    with open('README.md') as f:
        return f.read()

def dependencies(file):
    with open(file) as f:
        return f.read().splitlines()

setup(
    name='halo',
    packages=find_packages(exclude=('tests', 'examples')),
    version='0.0.2',
    license='MIT',
    description='Beautiful terminal spinners in Python',
    long_description=readme(),
    author='Manraj Singh',
    author_email='manrajsinghgrover@gmail.com',
    url='https://github.com/ManrajGrover/halo',
    keywords=[
        "console",
        "loading",
        "indicator",
        "progress",
        "cli",
        "spinner",
        "spinners",
        "terminal",
        "term",
        "busy",
        "wait",
        "idle"
    ],
    install_requires=dependencies('requirements.txt'),
    tests_require=dependencies('requirements-dev.txt'),
    include_package_data=True
)
