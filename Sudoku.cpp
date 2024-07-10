#include <vector>
#include <iostream>

bool checkSudoku(std::vector<int> solution)
{
    for (int i = 0; i < 9; i++)
    {
        bool m[9] = { 0,0,0,0,0,0,0,0,0 };
        for (int j = 0; j < 9; j++)
        {
            if (m[solution[i * 9 + j] - 1])
                return false;
            m[solution[i * 9 + j] - 1] = 1;
        }
    }
    for (int i = 0; i < 9; i++)
    {
        bool m[9] = { 0,0,0,0,0,0,0,0,0 };
        for (int j = 0; j < 9; j++)
        {
            if (m[solution[i + j * 9] - 1])
                return false;
            m[solution[i + j * 9] - 1] = 1;
        }
    }
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            bool m[9] = { 0,0,0,0,0,0,0,0,0 };
            for (int ij = 0; ij < 3; ij++)
            {
                for (int ji = 0; ji < 3; ji++)
                {
                    if (m[solution[((i*3)+ij) * 9 + j*3 + ji] - 1])
                        return false;
                    m[solution[((i * 3) + ij) * 9 + j*3 + ji] - 1] = 1;
                }
            }
        }
    }
    return true;
}

std::vector<int> nextSolution(std::vector<int> task, std::vector<int> oldSolution)
{
    std::vector<int> filtredSolution;
    for (int i = 0; i < 81; i++)
    {
        if (task[i] == 0)
        {
            filtredSolution.push_back(oldSolution[i]);
        }
    }
    if (filtredSolution.size() == 0)
        return task;
    for (int i = 0; i < filtredSolution.size(); i++)
    {
        if (filtredSolution[i] != 9) break;
        if (filtredSolution[i] == 9 && i == filtredSolution.size() - 1)
            return task;
    }

    int i = filtredSolution.size() - 1;
    if (filtredSolution[i] == 9)
    {
        while (filtredSolution[i] == 9 && i >= 0)
        {
            filtredSolution[i] = 1;
            i--;
        }
        if (i < 0)
            return task;
        else
            filtredSolution[i]++;
    }
    else filtredSolution[i]++;

    for (int itt = 0; itt < filtredSolution.size(); itt++) std::cout << filtredSolution[itt];
    std::cout << std::endl;

    std::vector<int> nSolution;
    int ji = 0;
    for (int ij = 0; ij < 81; ij++)
    {
        if (task[ij] == 0)
        {
            nSolution.push_back(filtredSolution[ji]);
            ji++;
        }
        else nSolution.push_back(task[ij]);
    }
    return nSolution;
}

int main()
{
    std::vector<int> task;
    for (int i = 0; i < 81; i++)
    {
        int t = 0;
        //std::cin >> t;
        task.push_back(t);
    }
    std::vector<int> solution = task;

    for (int i = 0; i < 81; i++)
    {
        if (solution[i] == 0)
            solution[i] = 1;
    }
 
    while (!checkSudoku(solution))
    {
        solution = nextSolution(task, solution);
        if (solution == task)
            break;
    }

    for (int i = 0; i < 81; i++)
    {
        if (i % 9 == 0)
            std::cout << std::endl;
        std::cout << solution[i] << " ";
    }
    std::cout << std::endl;
    system("pause");
}
