import ru from './ru';
import en from './en';

export type Locale = 'ru' | 'en';
export type Key = keyof typeof ru;
export type Translate = (key: Key) => string;

const dictionaries = { ru, en };
export const translate = (locale: Locale): Translate => key => dictionaries[locale][key];
export const errorMessage = (t: Translate, code: string) =>
  t(Object.hasOwn(en, code) ? code as Key : 'conversionFailed');
