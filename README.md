# Energy Efficiency Tests
## Goals
- The goal of these tests is to model whether dynamically inserting nop instructions at runtime could be used as a method of reducing the amount of wattage used by a CPU when executing instructions.
## Background
- These programs are designed to run on Intel CPUs running Linux OS. Testing for these experiments was done on a 12700KF Intel CPU running Fedora 41 Linux. Due to the nature of the program requiring read access to Intel's RAPL Register for calculating wattage, it is not known if it is possible to run this program on a system that does not meet these two requirements.
- An additional requirement of running these tests is that the program be run on bare metal using root level privileges. Root level privileges are required for reading the RAPL register and loading in the msr kernel module. Running these tests on bare metal is required to obtain access to the RAPL CPU register.
## Running the tests
- The first step to run the tests is to compile the program as so:
```
make
```
- Once the program is compiled, the run script can be called
```
./run.sh
```

- While running the program, it is important to minimize background processes in order to reduce noise. Under Fedora Linux, you can reduce most background processes by booting into the OS without starting a graphical session like so:
```
systemctl set-default multi-user.target
```
After executing the above command, restarting the machine will cause it to start in headless mode without GUI. The GUI can be re-enabled using:
```
systemctl set-default graphical.target
```
## Citation
- A lot of the code for this program was adapted from hertzbleed, who provided their source code under an open license
```
@inproceedings{wan2022hertzbleed,
    author = {Yingchen Wang and Riccardo Paccagnella and Elizabeth He and Hovav Shacham and Christopher W. Fletcher and David Kohlbrenner},
    title = {Hertzbleed: Turning Power Side-Channel Attacks Into Remote Timing Attacks on x86},
    booktitle = {Proc.\ of the USENIX Security Symposium (USENIX)},
    year = {2022},
}
```
