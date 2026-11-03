#pragma once

namespace notepalette {
constexpr int Count=21;
inline int Pitch(int slot) {
    const int offsets[]={0,2,4,5,7,9,11};
    return slot>=0 && slot<Count ? 60+(1-slot/7)*12+offsets[slot%7]:-1;
}
inline int RowPitch(int row) {return row>=0 && row<Count ? Pitch((row/7)*7+6-row%7):-1;}
inline char RowKey(int row) {
    const char* keys[]={"QWERTYU","ASDFGHJ","ZXCVBNM"};
    return row>=0 && row<Count ? keys[row/7][6-row%7]:'?';
}
inline const char* RowName(int row) {
    const char* names[]={"do","re","mi","fa","so","la","ti"};
    return names[6-row%7];
}
inline const char* RowLabel(int row) {
    const char* names[3][7]={{"↑do 1","↑re 2","↑mi 3","↑fa 4","↑so 5","↑la 6","↑ti 7"},
        {"do 1","re 2","mi 3","fa 4","so 5","la 6","ti 7"},{"↓do 1","↓re 2","↓mi 3","↓fa 4","↓so 5","↓la 6","↓ti 7"}};
    return row>=0 && row<Count ? names[row/7][6-row%7]:"?";
}
inline int Row(int pitch) {for (int row=0;row<Count;++row) if (RowPitch(row)==pitch) return row;return -1;}
}
