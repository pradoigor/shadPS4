// SPDX-License-Identifier: GPL-2.0-or-later
RWStructuredBuffer<uint> Result : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { Result[id.x] = 0x53484144; }
