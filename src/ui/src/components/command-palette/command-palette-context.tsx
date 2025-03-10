/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as React from 'react';

import { SetStateFunc } from 'app/context/common';
import { WithChildren } from 'app/utils/react-boilerplate';

import { useCommandProviders } from './command-providers';
import { parse, Token, SelectedToken } from './parser';
import { CommandCompletion, CommandCta } from './providers/command-provider';

export interface CommandPaletteContextProps {
  open: boolean;
  setOpen: SetStateFunc<boolean>;
  inputValue: string;
  setInputValue: SetStateFunc<string>;
  selection: [start: number, end: number]; // Where the caret or active selection is
  setSelection: SetStateFunc<[start: number, end: number]>;
  tokens: Token[];
  selectedTokens: SelectedToken[];
  completions: CommandCompletion[];
  highlightedCompletion: CommandCompletion | null;
  setHighlightedCompletion: SetStateFunc<CommandCompletion>;
  activateCompletion: (option: CommandCompletion) => void;
  cta: CommandCta | null;
}

export const CommandPaletteContext = React.createContext<CommandPaletteContextProps>({
  open: false,
  setOpen: () => { throw new Error('CommandPaletteContext is not in scope'); },
  inputValue: '',
  setInputValue: () => { throw new Error('CommandPaletteContext is not in scope'); },
  selection: [0, 0],
  setSelection: () => { throw new Error('CommandPaletteContext is not in scope'); },
  tokens: [],
  selectedTokens: [],
  completions: [],
  highlightedCompletion: null,
  setHighlightedCompletion: () => { throw new Error('CommandPaletteContext is not in scope'); },
  activateCompletion: () => { throw new Error('CommandPaletteContext is not in scope'); },
  cta: null,
});

const CommandPaletteContextProviderInner = React.memo<WithChildren>(({
  children,
}) => {
  // These need to be available to suggesters and CTAs, so they're defined in a wrapper that sets them up
  const { open, setOpen } = React.useContext(CommandPaletteContext);
  const [inputValue, setInputValue] = React.useState('');
  const [selection, setSelection] = React.useState<[start: number, end: number]>([0, 0]);
  const [highlightedCompletion, setHighlightedCompletion] = React.useState<CommandCompletion>(null);

  const { tokens, selectedTokens } = parse(inputValue, selection);

  const groupedCompletions = useCommandProviders(inputValue, selection);
  const allCompletions = React.useMemo(() => groupedCompletions.map(g => g.completions).flat(), [groupedCompletions]);

  const activateCompletion = React.useCallback((option: CommandCompletion) => {
    if (typeof option === 'string' || !option?.key) {
      // This happens if pressing Enter while no option is highlighted.
      // We could activate the provider's action here, but the user probably doesn't want that.
      return;
    }

    const index = allCompletions.findIndex((other) => other.key === option.key);
    const isHighlight = option.key === highlightedCompletion?.key;

    if (index == null || !isHighlight) {
      console.warn('Tried to activate a completion that is either not in the list, or is not highlighted.');
      return;
    }

    const answer = option.onSelect?.();
    if (answer) {
      setInputValue(answer[0]);
      setSelection([answer[1], answer[1]]);
    }
  }, [allCompletions, highlightedCompletion]);

  const cta = React.useMemo(() => {
    if (highlightedCompletion?.cta) {
      return highlightedCompletion.cta;
    } else if (highlightedCompletion) {
      return groupedCompletions.find((res) => res.completions.includes(highlightedCompletion))?.cta ?? null;
    } else {
      // Pick the first matching provider that has a CTA, or if there are none, the first provider at all with a CTA.
      // Providers should not offer a CTA if they aren't relevant to the current input.
      return groupedCompletions.find((res) => res.completions.length > 0 && res.cta)?.cta
        ?? groupedCompletions.find((res) => res.cta)?.cta
        ?? null;
    }
  }, [groupedCompletions, highlightedCompletion]);

  const ctx = React.useMemo(() => ({
    open,
    setOpen,
    inputValue,
    setInputValue,
    selection,
    setSelection,
    tokens,
    selectedTokens,
    completions: allCompletions,
    highlightedCompletion,
    setHighlightedCompletion,
    activateCompletion,
    cta,
  }), [
    open, setOpen, inputValue, selection,
    tokens, selectedTokens, cta,
    allCompletions, highlightedCompletion, activateCompletion,
  ]);
  return <CommandPaletteContext.Provider value={ctx}>{children}</CommandPaletteContext.Provider>;
});
CommandPaletteContextProviderInner.displayName = 'CommandPaletteContextProviderInner';

// Wraps the real provider in order to provide things like open/setOpen to the command completion providers' CTAs.
export const CommandPaletteContextProvider = React.memo<WithChildren>(({ children }) => {
  const defaults = React.useContext(CommandPaletteContext);
  const [open, setOpen] = React.useState(false);
  const ctx = React.useMemo(() => ({ ...defaults, open, setOpen }), [defaults, open, setOpen]);
  return (
    <CommandPaletteContext.Provider value={ctx}>
      <CommandPaletteContextProviderInner>
        {children}
      </CommandPaletteContextProviderInner>
    </CommandPaletteContext.Provider>
  );
});
CommandPaletteContextProvider.displayName = 'CommandPaletteContextProviderOuter';
